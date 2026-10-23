// Toolbar badge per tab: ON while Hush is processing the mic, a red dot
// while it is muting, ! if it could not start (the call still works).
"use strict";
const LOOK = {
  active: { text: "ON", color: "#0f7c7a" },
  muting: { text: "●", color: "#c8463b" },
  paused: { text: "OFF", color: "#6a7a88" },
  error: { text: "!", color: "#b7791f" },
  idle: { text: "", color: "#0f7c7a" },
};

chrome.runtime.onMessage.addListener((msg, sender) => {
  if (!msg || msg.type !== "badge" || !sender.tab) return;
  const key = msg.state === "active" && msg.muted ? "muting" : msg.state;
  const look = LOOK[key] || LOOK.idle;
  chrome.action.setBadgeText({ tabId: sender.tab.id, text: look.text });
  chrome.action.setBadgeBackgroundColor({ tabId: sender.tab.id, color: look.color });
});
