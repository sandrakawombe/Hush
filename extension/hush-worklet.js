// Hush audio processor. Runs on the browser's audio thread and passes every
// 128-sample block of microphone audio through the Hush engine (WebAssembly).
//
// Options (processorOptions):
//   wasmBytes     ArrayBuffer with hush.wasm
//   lookaheadMs   look-ahead buffer (default 100)
//   detector      2 = trained model (default), 1 = rules, 0 = off
//   enabled       start enabled (default true)
//
// Messages it sends on its port:
//   {type: "ready", latencyMs}        engine running
//   {type: "muted", muted: bool}      only when it changes
//   {type: "stats", ...}              about once a second
//   {type: "error", message}          engine failed: audio passes through untouched
// Messages it accepts: {type: "enabled", value: bool}
"use strict";

const WASI_STUBS = {
  // hush.wasm never writes files; these exist only because libc links them.
  wasi_snapshot_preview1: {
    fd_write: () => 8,  // EBADF
    fd_seek: () => 8,
    fd_close: () => 8,
  },
};

class HushProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};
    this.ok = false;
    this.muted = false;
    this.blocks = 0;
    this.statsEvery = Math.max(1, Math.round(sampleRate / 128));  // ~1 s
    this.port.onmessage = (e) => this.onMessage(e.data);
    try {
      const module = new WebAssembly.Module(o.wasmBytes);
      const inst = new WebAssembly.Instance(module, WASI_STUBS);
      this.x = inst.exports;
      if (this.x._initialize) this.x._initialize();
      this.block = 128;
      this.h = this.x.hush_wasm_create(sampleRate, this.block, o.lookaheadMs ?? 100, o.detector ?? 2);
      if (!this.h) throw new Error("engine rejected the settings (sample rate " + sampleRate + ")");
      this.inPtr = this.x.hush_wasm_input(this.h);
      this.outPtr = this.x.hush_wasm_output(this.h);
      this.views();
      this.x.hush_wasm_set_enabled(this.h, o.enabled === false ? 0 : 1);
      this.ok = true;
      this.port.postMessage({ type: "ready", latencyMs: (1000 * this.x.hush_wasm_latency(this.h)) / sampleRate, sampleRate });
    } catch (err) {
      this.port.postMessage({ type: "error", message: String((err && err.message) || err) });
    }
  }

  views() {
    this.mem = this.x.memory.buffer;
    this.inView = new Float32Array(this.mem, this.inPtr, this.block);
    this.outView = new Float32Array(this.mem, this.outPtr, this.block);
  }

  onMessage(msg) {
    if (!msg || !this.ok) return;
    if (msg.type === "enabled") this.x.hush_wasm_set_enabled(this.h, msg.value ? 1 : 0);
  }

  process(inputs, outputs) {
    const input = inputs[0] || [];
    const output = outputs[0] || [];
    const n = (output[0] && output[0].length) || (input[0] && input[0].length) || 128;

    if (!this.ok || n > this.block) {  // never break the call: pass audio straight through
      for (let c = 0; c < output.length; c++) {
        if (input[c]) output[c].set(input[c]);
        else if (input[0]) output[c].set(input[0]);
      }
      return true;
    }
    if (this.mem !== this.x.memory.buffer) this.views();  // memory grew: refresh views

    // mono mix of the microphone (silence if the input is momentarily absent)
    const inView = this.inView;
    if (input.length === 0) inView.fill(0, 0, n);
    else if (input.length === 1) inView.set(input[0].subarray(0, n));
    else {
      for (let i = 0; i < n; i++) {
        let s = 0;
        for (let c = 0; c < input.length; c++) s += input[c][i];
        inView[i] = s / input.length;
      }
    }

    const muted = this.x.hush_wasm_process(this.h, n);
    if (muted < 0) {
      this.ok = false;
      this.port.postMessage({ type: "error", message: "engine returned an error" });
      return true;
    }
    for (let c = 0; c < output.length; c++) output[c].set(this.outView.subarray(0, n));

    if ((muted === 1) !== this.muted) {
      this.muted = muted === 1;
      this.port.postMessage({ type: "muted", muted: this.muted });
    }
    if (++this.blocks % this.statsEvery === 0) {
      const p = this.x.hush_wasm_stats(this.h) >> 3;
      const s = new Float64Array(this.x.memory.buffer).subarray(p, p + 6);
      this.port.postMessage({ type: "stats", seconds: s[0] / sampleRate, mutes: s[1],
        leakedMs: (1000 * s[2]) / sampleRate, late: s[3], forced: s[4] });
    }
    return true;
  }
}

registerProcessor("hush-processor", HushProcessor);
