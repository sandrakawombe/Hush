"use strict";
const $ = (id) => document.getElementById(id);
const SECONDS = 15;
const cv = $("meter"), g = cv.getContext("2d");
const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();

function fail(msg) {
  $("error").textContent = msg;
  $("error").classList.remove("hidden");
  $("start").disabled = false;
  $("phase").textContent = "Ready";
}

function recorder(stream) {
  const chunks = [];
  const r = new MediaRecorder(stream);
  r.ondataavailable = (e) => e.data.size && chunks.push(e.data);
  const done = new Promise((res) => { r.onstop = () => res(new Blob(chunks, { type: r.mimeType })); });
  r.start();
  return { stop: () => { r.stop(); return done; } };
}

$("start").addEventListener("click", async () => {
  $("error").classList.add("hidden");
  $("results").classList.add("hidden");
  $("start").disabled = true;
  g.clearRect(0, 0, cv.width, cv.height);

  let mic;
  try {
    mic = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: true } });
  } catch (e) {
    return fail("Microphone access was blocked. Allow it from the icon in the address bar and try again.");
  }

  let muted = false, mutes = 0, mutedSince = 0, mutedTotal = 0, stats = null;
  const t0 = performance.now();
  let handle;
  try {
    const [wasmBytes, workletSource] = await Promise.all([
      fetch("hush.wasm").then((r) => r.arrayBuffer()),
      fetch("hush-worklet.js").then((r) => r.text()),
    ]);
    handle = await HushCore.wrap(mic, {
      wasmBytes, workletUrl: "hush-worklet.js", workletSource,
      onEvent: (m) => {
        const now = (performance.now() - t0) / 1000;
        if (m.type === "muted") {
          muted = m.muted;
          if (m.muted) { mutes++; mutedSince = now; } else mutedTotal += now - mutedSince;
          $("live").textContent = m.muted ? "Hush is muting" : "";
        } else if (m.type === "stats") stats = m;
      },
    });
  } catch (e) {
    mic.getTracks().forEach((t) => t.stop());
    return fail("Hush could not start: " + e.message);
  }

  // record both: the raw mic and what Hush sends on
  const rawRec = recorder(new MediaStream([mic.getAudioTracks()[0].clone()]));
  const hushRec = recorder(handle.stream);
  $("sDelay").textContent = Math.round(handle.info.latencyMs) + " ms";

  // level meter with muted moments in red
  const ac = new AudioContext();
  const an = ac.createAnalyser();
  an.fftSize = 1024;
  ac.createMediaStreamSource(new MediaStream([mic.getAudioTracks()[0].clone()])).connect(an);
  const buf = new Float32Array(an.fftSize);
  $("phase").textContent = "Recording… talk, then cough or sneeze";
  const tick = () => {
    const el = (performance.now() - t0) / 1000;
    $("clock").textContent = Math.max(0, Math.ceil(SECONDS - el));
    an.getFloatTimeDomainData(buf);
    let peak = 0;
    for (const v of buf) peak = Math.max(peak, Math.abs(v));
    const x = Math.floor((el / SECONDS) * cv.width), h = Math.min(1, peak * 3) * cv.height;
    g.fillStyle = muted ? css("--mute") : css("--signal");
    g.fillRect(x, (cv.height - h) / 2, Math.max(2, cv.width / (SECONDS * 60)), Math.max(2, h));
    if (el < SECONDS) requestAnimationFrame(tick);
    else finish();
  };
  requestAnimationFrame(tick);

  async function finish() {
    $("phase").textContent = "Done";
    $("clock").textContent = "0";
    if (muted) mutedTotal += (performance.now() - t0) / 1000 - mutedSince;
    const [raw, hushed] = await Promise.all([rawRec.stop(), hushRec.stop()]);
    handle.stop();
    ac.close();
    $("raw").src = URL.createObjectURL(raw);
    $("hushed").src = URL.createObjectURL(hushed);
    $("sMutes").textContent = String(mutes);
    $("sMuted").textContent = mutedTotal.toFixed(1) + " s";
    $("sLeak").textContent = stats ? Math.round(stats.leakedMs) + " ms" : "0 ms";
    $("results").classList.remove("hidden");
    $("start").disabled = false;
    $("start").textContent = "Record again";
    $("hint").textContent = mutes ? "Play both to compare." : "No cough was detected. Try coughing closer to the mic.";
  }
});
