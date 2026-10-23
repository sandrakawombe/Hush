// Runs inside the meeting page (MAIN world). Wraps getUserMedia so the
// microphone audio the page receives has passed through Hush.
//
// The extension's own files and settings come from bridge.js (the isolated
// content script) through window messages tagged __hush. If anything goes
// wrong, the page gets the original microphone, untouched.
"use strict";
(function () {
  const md = navigator.mediaDevices;
  if (!md || !md.getUserMedia || md.__hushWrapped) return;
  const original = md.getUserMedia.bind(md);
  Object.defineProperty(md, "__hushWrapped", { value: true });

  let enabled = true;
  let assets = null;          // {wasmBytes, workletUrl, workletSource}
  let assetsWaiters = [];
  const live = new Set();     // active wrapped streams on this page

  function send(msg) {
    window.postMessage(Object.assign({ __hush: "page" }, msg), "*");
  }

  window.addEventListener("message", (e) => {
    if (e.source !== window || !e.data || e.data.__hush !== "bridge") return;
    const m = e.data;
    if (m.type === "assets") {
      assets = m.assets;
      assetsWaiters.forEach((w) => w(assets));
      assetsWaiters = [];
    } else if (m.type === "enabled") {
      enabled = !!m.value;
      live.forEach((h) => h.setEnabled(enabled));
      send({ type: "status", state: live.size ? (enabled ? "active" : "paused") : "idle" });
    }
  });

  function getAssets() {
    if (assets) return Promise.resolve(assets);
    send({ type: "need-assets" });
    return new Promise((resolve, reject) => {
      assetsWaiters.push(resolve);
      setTimeout(() => reject(new Error("extension files did not arrive")), 3000);
    });
  }

  function wantsAudio(c) {
    return !!(c && c.audio);
  }

  md.getUserMedia = async function (constraints) {
    const stream = await original(constraints);
    if (!wantsAudio(constraints) || !enabled) return stream;
    try {
      const a = await getAssets();
      let muteCount = 0;
      const handle = await HushCore.wrap(stream, {
        wasmBytes: a.wasmBytes.slice(0),
        workletUrl: a.workletUrl,
        workletSource: a.workletSource,
        enabled,
        onEvent: (m) => {
          if (m.type === "muted") {
            if (m.muted) muteCount++;
            send({ type: "muted", muted: m.muted, count: muteCount });
          } else if (m.type === "stats") {
            send({ type: "stats", stats: m });
          } else if (m.type === "error") {
            send({ type: "status", state: "error", message: m.message });
          } else if (m.type === "stopped") {
            live.delete(handle);
            send({ type: "status", state: live.size ? "active" : "idle" });
          }
        },
      });
      live.add(handle);
      send({ type: "status", state: "active", latencyMs: handle.info.latencyMs, sampleRate: handle.info.sampleRate });
      return handle.stream;
    } catch (err) {
      send({ type: "status", state: "error", message: String((err && err.message) || err) });
      return stream;  // never break the call
    }
  };
})();
