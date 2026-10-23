// Tests the in-page microphone wrapper (hush-core.js + inject.js) with a
// fake browser. The key promise: whatever fails, the page still gets a
// working microphone.
//
//   node extension/test/inject.test.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const ext = join(dirname(fileURLToPath(import.meta.url)), "..");
const src = (f) => readFileSync(join(ext, f), "utf8");
const wasm = readFileSync(join(ext, "hush.wasm"));
const wasmBytes = wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.length);

class FakeTrack {
  constructor(kind, label) { this.kind = kind; this.label = label; this.stopped = false; this.listeners = {}; }
  stop() { this.stopped = true; }
  getSettings() { return { deviceId: "mic-1", sampleRate: 48000 }; }
  getCapabilities() { return {}; }
  getConstraints() { return {}; }
  applyConstraints() { return Promise.resolve(); }
  clone() { return new FakeTrack(this.kind, this.label); }
  addEventListener(t, f) { (this.listeners[t] ||= []).push(f); }
}
class FakeStream {
  constructor(tracks = []) { this.tracks = tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
  getTracks() { return this.tracks; }
}

// The real worklet, running the real hush.wasm, behind a fake AudioWorkletNode.
function workletClass() {
  let Proc;
  vm.runInNewContext(src("hush-worklet.js"), {
    sampleRate: 48000,
    // Like a real MessagePort: messages sent before anyone listens are queued.
    AudioWorkletProcessor: class { constructor() { const q = []; this._queue = q;
      this.port = { postMessage: (m) => (this._out ? this._out(m) : q.push(m)), onmessage: null }; } },
    registerProcessor: (_n, c) => { Proc = c; },
    WebAssembly, Float32Array, Float64Array, Math, String, Error,
  });
  return Proc;
}

function makeBrowser({ audioContext = "ok", workletFails = false, bridge = true } = {}) {
  const listeners = [];
  const posted = [];
  const window = {
    postMessage(data) {
      posted.push(data);
      const ev = { source: window, data };
      setTimeout(() => listeners.forEach((l) => l(ev)), 0);
    },
    addEventListener(type, fn) { if (type === "message") listeners.push(fn); },
  };
  const realMic = new FakeTrack("audio", "Built-in Microphone");
  const camera = new FakeTrack("video", "FaceTime Camera");
  const navigator = {
    mediaDevices: {
      getUserMedia: async (c) => new FakeStream([...(c.audio ? [realMic] : []), ...(c.video ? [camera] : [])]),
    },
  };
  const Proc = workletClass();
  let processor = null;
  class AudioContext {
    constructor() { this.state = audioContext === "suspended" ? "suspended" : "running"; this.closed = false;
      this.audioWorklet = { addModule: async () => { if (workletFails) throw new Error("blocked by page"); } }; }
    resume() { return Promise.resolve(); }
    close() { this.closed = true; return Promise.resolve(); }
    createMediaStreamSource() { return { connect: (n) => n }; }
    createMediaStreamDestination() { return { stream: new FakeStream([new FakeTrack("audio", "MediaStreamAudioDestinationNode")]) }; }
  }
  class AudioWorkletNode {
    constructor(_ctx, _name, opts) {
      this.port = { onmessage: null, postMessage: (m) => processor.port.onmessage && processor.port.onmessage({ data: m }) };
      processor = new Proc(opts);
      processor._out = (m) => setTimeout(() => this.port.onmessage && this.port.onmessage({ data: m }), 0);
      processor._queue.splice(0).forEach((m) => processor._out(m));  // deliver queued messages
    }
    connect(n) { return n; }
    disconnect() {}
  }
  if (audioContext === "missing") { /* no AudioContext at all */ }
  const g = {
    window, navigator, setTimeout, clearTimeout, URL, Blob: class {}, Promise, Object, String, Error, Set,
    MediaStream: FakeStream, console,
    ...(audioContext === "missing" ? {} : { AudioContext, AudioWorkletNode }),
  };
  g.globalThis = g;
  vm.createContext(g);
  vm.runInContext(src("hush-core.js"), g);
  vm.runInContext(src("inject.js"), g);
  if (bridge) {
    listeners.push((e) => {
      if (e.data.__hush === "page" && e.data.type === "need-assets") {
        window.postMessage({ __hush: "bridge", type: "assets", assets: { wasmBytes, workletUrl: "hush-worklet.js", workletSource: "" } });
      }
    });
  }
  return { g, posted, realMic, camera, getProcessor: () => processor };
}


const tests = [];
const test = (name, fn) => tests.push([name, fn]);

test("video-only requests are left alone", async () => {
  const b = makeBrowser();
  const s = await b.g.navigator.mediaDevices.getUserMedia({ video: true });
  assert.equal(s.getVideoTracks()[0], b.camera);
  assert.equal(s.getAudioTracks().length, 0);
});

test("no AudioWorklet support: page gets the real mic", async () => {
  const b = makeBrowser({ audioContext: "missing" });
  const s = await b.g.navigator.mediaDevices.getUserMedia({ audio: true });
  assert.equal(s.getAudioTracks()[0], b.realMic);
  assert.ok(b.posted.some((m) => m.type === "status" && m.state === "error"));
});

test("audio not allowed to start: page gets the real mic", async () => {
  const b = makeBrowser({ audioContext: "suspended" });
  const s = await b.g.navigator.mediaDevices.getUserMedia({ audio: true });
  assert.equal(s.getAudioTracks()[0], b.realMic);
});

test("worklet blocked by the page: page gets the real mic", async () => {
  const b = makeBrowser({ workletFails: true });
  const s = await b.g.navigator.mediaDevices.getUserMedia({ audio: true });
  assert.equal(s.getAudioTracks()[0], b.realMic);
});

test("extension files never arrive: page gets the real mic", async () => {
  const b = makeBrowser({ bridge: false });
  const s = await b.g.navigator.mediaDevices.getUserMedia({ audio: true, video: true });
  assert.equal(s.getAudioTracks()[0], b.realMic);
  assert.equal(s.getVideoTracks()[0], b.camera);
});

test("happy path: processed mic, camera kept, looks like the real mic, stops cleanly", async () => {
  const b = makeBrowser();
  const s = await b.g.navigator.mediaDevices.getUserMedia({ audio: true, video: true });
  const a = s.getAudioTracks()[0];
  assert.notEqual(a, b.realMic, "audio should be the processed track");
  assert.equal(s.getVideoTracks()[0], b.camera);
  assert.equal(a.label, "Built-in Microphone");
  assert.equal(a.getSettings().deviceId, "mic-1");
  assert.ok(b.posted.some((m) => m.type === "status" && m.state === "active" && m.latencyMs === 100));
  // the real engine is running inside: feed it a block
  const p = b.getProcessor();
  const out = [new Float32Array(128)];
  assert.equal(p.process([[new Float32Array(128).fill(0.1)]], [out]), true);
  a.stop();
  assert.equal(b.realMic.stopped, true, "stopping the processed track releases the real mic");
});

test("turning Hush off before the call leaves the mic untouched", async () => {
  const b = makeBrowser();
  b.g.window.postMessage({ __hush: "bridge", type: "enabled", value: false });
  await new Promise((r) => setTimeout(r, 10));
  const s = await b.g.navigator.mediaDevices.getUserMedia({ audio: true });
  assert.equal(s.getAudioTracks()[0], b.realMic);
});

let failed = 0;
for (const [name, fn] of tests) {
  try {
    await fn();
    console.log("PASS", name);
  } catch (e) {
    failed++;
    console.log("FAIL", name, "\n   ", e.message);
  }
}
console.log(`\n${tests.length - failed} of ${tests.length} passed`);
process.exit(failed ? 1 : 0);
