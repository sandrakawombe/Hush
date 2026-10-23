"use strict";
const $ = (id) => document.getElementById(id);

chrome.storage.sync.get({ enabled: true }, (s) => { $("enabled").checked = s.enabled; });
$("enabled").addEventListener("change", (e) => {
  chrome.storage.sync.set({ enabled: e.target.checked });
  refresh();
});

function show(state, detail, tone) {
  $("state").textContent = state;
  $("detail").textContent = detail || "";
  $("dot").dataset.tone = tone;
}

function refresh() {
  chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
    const tab = tabs[0];
    if (!tab) return show("No tab", "", "idle");
    chrome.tabs.sendMessage(tab.id, { type: "get-status" }, (s) => {
      if (chrome.runtime.lastError || !s) {
        return show("Not a meeting page", "Open Google Meet, Teams or Zoom in this tab.", "idle");
      }
      const on = $("enabled").checked;
      if (s.state === "active") {
        const n = s.count || 0;
        show(on ? "Protecting your mic" : "Paused for this call",
          `${n} cough${n === 1 ? "" : "s"} muted so far` + (s.latencyMs ? ` · ${Math.round(s.latencyMs)} ms delay` : ""),
          on ? (s.muted ? "muting" : "on") : "idle");
      } else if (s.state === "paused") {
        show("Paused for this call", "Turn the switch back on to resume muting.", "idle");
      } else if (s.state === "error") {
        show("Couldn't start here", (s.message || "Unknown problem") + ". Your mic works normally.", "warn");
      } else {
        show(on ? "Ready" : "Off", on ? "Hush starts when the page turns on your mic." : "New calls won't be processed.", on ? "on" : "idle");
      }
    });
  });
}
refresh();
setInterval(refresh, 1000);
