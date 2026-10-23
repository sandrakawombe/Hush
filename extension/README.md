# Hush for Chrome

Silences coughs and sneezes in **Google Meet, Microsoft Teams and Zoom** on
the web, then unmutes on its own. The same C++ engine and trained model as the
rest of the repo, compiled to WebAssembly. Everything runs on your computer;
no audio leaves the browser except what the meeting itself sends.

> Early test version. Load it yourself from this folder; it is not on the
> Chrome Web Store.

## Install (2 minutes)

1. Open `chrome://extensions` in Chrome (version 111 or newer).
2. Turn on **Developer mode** (top right).
3. Click **Load unpacked** and choose this `extension` folder.
4. Pin Hush from the puzzle-piece menu so you can see its badge.

## Try it before a real call

Click the Hush icon, then **Try it with your mic**. Record 15 seconds, talk,
cough or sneeze a couple of times, then play back *what your mic heard* next to
*what the call hears*. The level bar turns red where Hush muted.

## Use it in a meeting

Turn Hush on (it is on by default), then join a call. Hush starts when the
page turns on your microphone, so **join after switching it on**; turning it
on or off mid-call takes effect from your next call (turning it off mid-call
stops the muting right away, but the small delay stays until you rejoin).

A safe place to test: start a Google Meet with only yourself
(`meet.google.com/new`) and use its *Check your audio and video* screen, or a
Zoom test meeting (`zoom.us/test`, then *Join from your browser*).

| Badge | Meaning |
|---|---|
| **ON** | Hush is processing your mic |
| **●** (red) | Muting a cough or sneeze right now |
| **OFF** | Paused for this call |
| **!** | Hush couldn't start on this page. **Your mic still works normally.** Click the icon to see why. |

## How it works

```
your mic ─► getUserMedia (wrapped by Hush) ─► audio worklet ─► hush.wasm ─► the meeting
                                               (100 ms look-ahead, mute + fade)
```

* `inject.js` wraps `navigator.mediaDevices.getUserMedia` in the meeting page.
  When the page asks for the mic, it gets a processed copy instead; video is
  passed through untouched.
* `hush-worklet.js` runs `hush.wasm` on the browser's audio thread, 128
  samples at a time.
* **If anything fails** (the page blocks the worklet, audio isn't allowed to
  start yet, the engine can't load), the page simply gets the original
  microphone. Hush cannot leave you without a mic.
* `bridge.js` passes the extension's files and the on/off setting into the
  page, and reports status to the toolbar badge.

## Tested so far

| What | Result |
|---|---|
| Browser engine vs native C++ engine, all 120 bench calls | identical output (within 1 LSB) |
| Same at 48 kHz and 44.1 kHz | identical |
| Mic wrapper in a simulated browser: 5 failure cases + success path | page always gets a working mic |
| **Real Meet / Teams / Zoom in Chrome** | **not yet tested; please try and report** |

Things that could differ on real sites, and what you'd see:

* A site's security policy may block the audio worklet or WebAssembly. You'd
  see the **!** badge and your mic works normally.
* Teams and Zoom sometimes run the call inside frames; Hush is injected into
  all frames on those sites.
* Hush adds about **100 ms** of delay to your outgoing audio. Calls already
  carry some delay; tell us if you notice it.

## Rebuild the engine

`hush.wasm` is checked in so the extension works straight from the repo.
After changing the engine or retraining the model:

```bash
./extension/build.sh        # needs clang and lld; downloads the WASI SDK once
node extension/test/inject.test.mjs
node extension/test/parity.mjs path/to/*.wav
```
