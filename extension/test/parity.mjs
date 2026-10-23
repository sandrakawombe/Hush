// Runs the real extension worklet (hush-worklet.js + hush.wasm) in Node on
// WAV files and checks it produces the same audio as the native engine
// (engine/build/hush_run spans --out). Same code compiled two ways should
// give the same mutes; tiny math-library differences are allowed.
//
//   node extension/test/parity.mjs file1.wav file2.wav ...
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..");
const wasmBytes = readFileSync(join(here, "..", "hush.wasm"));
const workletSrc = readFileSync(join(here, "..", "hush-worklet.js"), "utf8");
const hushRun = join(root, "engine", "build", process.platform === "win32" ? "hush_run.exe" : "hush_run");

function readWav(path) {
  const b = readFileSync(path);
  let p = 12, rate = 0, ch = 1, data = null;
  while (p + 8 <= b.length) {
    const id = b.toString("ascii", p, p + 4), len = b.readUInt32LE(p + 4);
    if (id === "fmt ") { ch = b.readUInt16LE(p + 10); rate = b.readUInt32LE(p + 12); }
    if (id === "data") data = b.subarray(p + 8, p + 8 + len);
    p += 8 + len + (len & 1);
  }
  const n = data.length / (2 * ch), x = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let s = 0;
    for (let c = 0; c < ch; c++) s += data.readInt16LE(2 * (i * ch + c)) / 32767;
    x[i] = s / ch;
  }
  return { x, rate };
}

function runWorklet(x, rate) {
  // minimal AudioWorkletGlobalScope
  let Proc = null;
  const messages = [];
  const scope = {
    sampleRate: rate,
    AudioWorkletProcessor: class { constructor() { this.port = { postMessage: (m) => messages.push(m), onmessage: null }; } },
    registerProcessor: (_name, cls) => { Proc = cls; },
    WebAssembly, Float32Array, Float64Array, Math, String, Error,
  };
  vm.runInNewContext(workletSrc, scope);
  const proc = new Proc({ processorOptions: { wasmBytes: wasmBytes.buffer.slice(wasmBytes.byteOffset, wasmBytes.byteOffset + wasmBytes.length) } });
  const ready = messages.find((m) => m.type === "ready");
  if (!ready) throw new Error("worklet did not start: " + JSON.stringify(messages));
  const D = Math.round((ready.latencyMs * rate) / 1000);
  const total = x.length + D, y = new Float32Array(total);
  const inBuf = new Float32Array(128), outBuf = new Float32Array(128);
  for (let i = 0; i < total; i += 128) {
    const n = Math.min(128, total - i);
    inBuf.fill(0);
    inBuf.set(x.subarray(Math.min(i, x.length), Math.min(i + n, x.length)));
    proc.process([[inBuf]], [[outBuf]]);
    y.set(outBuf.subarray(0, n), i);
  }
  return { y: y.subarray(D, D + x.length), messages };
}

const files = process.argv.slice(2);
if (!files.length) { console.error("usage: node extension/test/parity.mjs file.wav ..."); process.exit(2); }
const outDir = mkdtempSync(join(tmpdir(), "hush-parity-"));
execFileSync(hushRun, ["spans", "--block", "128", "--out", outDir, ...files], { stdio: ["ignore", "ignore", "inherit"] });

let worst = 0, same = 0, mismatchedFiles = [];
for (const f of files) {
  const { x, rate } = readWav(f);
  const { y } = runWorklet(x, rate);
  const ref = readWav(join(outDir, basename(f))).x;
  let bad = 0, maxd = 0;
  for (let i = 0; i < x.length; i++) {
    const q = Math.round(Math.max(-1, Math.min(1, y[i])) * 32767) / 32767;
    const d = Math.abs(q - ref[i]);
    maxd = Math.max(maxd, d);
    if (d > 1.5 / 32767) bad++;
  }
  worst = Math.max(worst, maxd);
  if (bad === 0) same++;
  else mismatchedFiles.push(`${basename(f)}: ${bad} samples differ (max ${(maxd * 32767).toFixed(0)} LSB)`);
}
console.log(`${same} of ${files.length} calls identical to the native engine (within 1 LSB)`);
for (const m of mismatchedFiles.slice(0, 10)) console.log("  " + m);
process.exit(same === files.length ? 0 : 1);
