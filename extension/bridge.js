// Isolated content script: hands the extension's files and the on/off
// setting to inject.js (which runs in the page), and reports the page's Hush
// status to the toolbar badge and popup.
"use strict";
(function () {
  let latest = { state: "idle", count: 0 };
  let assetsPromise = null;

  function toPage(msg) {
    window.postMessage(Object.assign({ __hush: "bridge" }, msg), "*");
  }

  function loadAssets() {
    if (!assetsPromise) {
      const workletUrl = chrome.runtime.getURL("hush-worklet.js");
      assetsPromise = Promise.all([
        fetch(chrome.runtime.getURL("hush.wasm")).then((r) => r.arrayBuffer()),
        fetch(workletUrl).then((r) => r.text()),
      ]).then(([wasmBytes, workletSource]) => ({ wasmBytes, workletUrl, workletSource }));
    }
    return assetsPromise;
  }

  function badge() {
    chrome.runtime.sendMessage({ type: "badge", state: latest.state, muted: !!latest.muted }).catch(() => {});
  }

  window.addEventListener("message", (e) => {
    if (e.source !== window || !e.data || e.data.__hush !== "page") return;
    const m = e.data;
    if (m.type === "need-assets") {
      loadAssets().then((assets) => toPage({ type: "assets", assets }), () => {});
    } else if (m.type === "status") {
      latest = Object.assign({}, latest, { state: m.state, message: m.message, latencyMs: m.latencyMs ?? latest.latencyMs });
      badge();
    } else if (m.type === "muted") {
      latest = Object.assign({}, latest, { muted: m.muted, count: m.count });
      badge();
    } else if (m.type === "stats") {
      latest = Object.assign({}, latest, { stats: m.stats });
    }
  });

  chrome.storage.sync.get({ enabled: true }, (s) => toPage({ type: "enabled", value: s.enabled }));
  chrome.storage.onChanged.addListener((changes) => {
    if (changes.enabled) toPage({ type: "enabled", value: changes.enabled.newValue });
  });

  chrome.runtime.onMessage.addListener((msg, _sender, reply) => {
    if (msg && msg.type === "get-status") reply(latest);
  });

  loadAssets();  // warm up so the first getUserMedia is fast
})();
