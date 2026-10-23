// Hush core: wraps a microphone MediaStream so its audio goes through the
// Hush engine. Used by the content script (inside meeting pages) and by the
// extension's demo page.
//
//   const out = await HushCore.wrap(micStream, {wasmBytes, workletUrl, enabled, onEvent});
//
// Contract: wrap() either returns a working processed stream or throws.
// Callers fall back to the original stream on any error, so Hush can never
// leave someone without a microphone.
"use strict";
(function (global) {
  if (global.HushCore) return;

  const READY_TIMEOUT_MS = 3000;

  function withTimeout(promise, ms, what) {
    return Promise.race([
      promise,
      new Promise((_, reject) => setTimeout(() => reject(new Error(what + " timed out")), ms)),
    ]);
  }

  async function loadWorklet(ctx, workletUrl, workletSource) {
    try {
      await ctx.audioWorklet.addModule(workletUrl);
    } catch (first) {
      if (!workletSource) throw first;
      // Some pages refuse to load scripts from the extension; try an in-memory copy.
      const url = URL.createObjectURL(new Blob([workletSource], { type: "text/javascript" }));
      try {
        await ctx.audioWorklet.addModule(url);
      } finally {
        URL.revokeObjectURL(url);
      }
    }
  }

  // Make the processed track look like the real microphone to the page.
  function disguise(track, real) {
    const pass = (name) => {
      if (typeof real[name] === "function") {
        Object.defineProperty(track, name, { configurable: true, value: (...a) => real[name](...a) });
      }
    };
    try {
      Object.defineProperty(track, "label", { configurable: true, get: () => real.label });
      pass("getCapabilities");
      pass("getConstraints");
      pass("applyConstraints");
      const ownSettings = track.getSettings ? track.getSettings.bind(track) : () => ({});
      Object.defineProperty(track, "getSettings", {
        configurable: true,
        value: () => Object.assign({}, ownSettings(), real.getSettings ? real.getSettings() : {}),
      });
    } catch (_) {
      /* cosmetic only */
    }
  }

  async function wrap(stream, opts) {
    const audio = stream.getAudioTracks();
    if (!audio.length) throw new Error("no microphone track");
    const onEvent = opts.onEvent || (() => {});
    const AC = global.AudioContext || global.webkitAudioContext;
    if (!AC || !global.AudioWorkletNode) throw new Error("this browser has no AudioWorklet");

    const ctx = new AC({ latencyHint: "interactive" });
    let node = null;
    try {
      if (ctx.state !== "running") await withTimeout(ctx.resume(), 1000, "starting audio");
      if (ctx.state !== "running") throw new Error("audio is not allowed to start on this page yet");
      await loadWorklet(ctx, opts.workletUrl, opts.workletSource);

      node = new AudioWorkletNode(ctx, "hush-processor", {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        outputChannelCount: [1],
        channelCount: 1,
        channelCountMode: "explicit",
        processorOptions: {
          wasmBytes: opts.wasmBytes,
          lookaheadMs: opts.lookaheadMs ?? 100,
          detector: opts.detector ?? 2,
          enabled: opts.enabled !== false,
        },
      });
      const ready = new Promise((resolve, reject) => {
        node.port.onmessage = (e) => {
          const m = e.data || {};
          if (m.type === "ready") resolve(m);
          else if (m.type === "error") reject(new Error(m.message));
          onEvent(m);
        };
      });
      const src = ctx.createMediaStreamSource(new MediaStream([audio[0]]));
      const dest = ctx.createMediaStreamDestination();
      src.connect(node).connect(dest);
      const info = await withTimeout(ready, READY_TIMEOUT_MS, "starting the engine");

      const track = dest.stream.getAudioTracks()[0];
      disguise(track, audio[0]);
      const stopOwn = track.stop.bind(track);
      let stopped = false;
      const shutdown = () => {
        if (stopped) return;
        stopped = true;
        stopOwn();
        audio.forEach((t) => t.stop());
        try { node.disconnect(); } catch (_) {}
        ctx.close().catch(() => {});
        onEvent({ type: "stopped" });
      };
      Object.defineProperty(track, "stop", { configurable: true, value: shutdown });
      audio[0].addEventListener("ended", shutdown);

      const out = new MediaStream([track, ...stream.getVideoTracks()]);
      return {
        stream: out,
        info,
        setEnabled: (on) => node.port.postMessage({ type: "enabled", value: !!on }),
        stop: shutdown,
      };
    } catch (err) {
      try { if (node) node.disconnect(); } catch (_) {}
      ctx.close().catch(() => {});
      throw err;
    }
  }

  Object.defineProperty(global, "HushCore", { value: { wrap }, configurable: false, enumerable: false });
})(globalThis);
