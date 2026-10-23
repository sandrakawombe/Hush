# Hush

Automatic cough and sneeze muting for calls and meetings. Hush listens a
fraction of a second ahead of what it sends, silences just the cough or
sneeze, and unmutes on its own.

> Status: early development. Pieces 1–3 of 6 are done: test bench, engine core, and the first detector running inside the engine.

## Plan

| # | Piece | Status |
|---|---|---|
| 1 | **Test bench**: labeled test calls, SQLite catalog, scoring | ✅ done |
| 2 | **Engine core** (C++): look-ahead buffer, detector slot, mute and fade, C API | ✅ done |
| 3 | **Rules detector in the engine**, scored on the bench | ✅ done |
| 4 | Chrome extension (Meet, Teams web, Zoom web) | |
| 5 | Trained on-device model in the detector slot | |
| 6 | Desktop virtual microphone, then Android demo | |

## The engine

```
mic ──► detector ──► decisions ─┐
 │                              ▼
 └──► look-ahead delay ───────► gate ──► call
```

The outgoing audio is held for `lookahead_ms` (100 ms by default). When the
detector decides "that was a cough, starting 60 ms ago", the gate can still
silence it, fading in and out over 15 ms so there are no clicks.

```bash
cd engine
cmake -S . -B build -G Ninja && cmake --build build && ./build/hush_tests
# memory and undefined-behavior checks:
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHUSH_SANITIZE=ON && cmake --build build-asan && ./build-asan/hush_tests
```

Guarantees, each backed by a test:

* **Never allocates memory** while processing audio (tested), and takes no locks.
* **Never stays muted** longer than `max_mute_ms` (2.5 s), even if the detector misbehaves.
* **Same output for any block size**, and in-place processing is safe.
* **Turning Hush off** keeps the same delay, so the audio never jumps.
* **Late decisions are counted**, not hidden: `leaked_samples` and `late_decisions` say exactly what got through.
* **Same decisions at any sample rate** (tested at 16 and 48 kHz): the detector always listens in 32 ms windows.
* With the rules detector, about **250× faster than real time** at 48 kHz on one core.

Run audio files through it:

```bash
./build/hush_run spans --lookahead-ms 100 call.wav        # what the engine muted, as JSON
./build/hush_run spans --out processed/ call.wav          # also write the processed audio
./build/hush_run proposals call.wav                       # what the detector proposed, and when
```

Call it from any platform through the plain C API in
[`engine/include/hush/hush.h`](engine/include/hush/hush.h). New detectors
implement the small interface in
[`engine/include/hush/detector.hpp`](engine/include/hush/detector.hpp).

## The test bench

Every change to Hush is judged on the same labeled test calls, so "better"
always means a number went the right way.

```bash
cd bench
pip install -r requirements.txt
./scripts/fetch_data.sh             # public clips, ~140 MB
python -m hush_bench.build          # 80 dev + 40 test calls -> data/scenes, data/catalog.db
python -m hush_bench.score --detector rules --split dev --lookahead-ms 100    # Python reference
python -m hush_bench.score --detector engine --split dev --lookahead-ms 100   # the real C++ engine
python -m hush_bench.parity --split dev                                       # C++ decides exactly like Python
python -m pytest -q tests
```

What the scores mean:

| Metric | Meaning | Goal |
|---|---|---|
| Caught | Coughs/sneezes with ≥ 80% of their first 300 ms muted | High |
| Leak | How late the mute started after the true onset | 0 ms |
| False mutes per hour | Mutes that touched no cough or sneeze | Near 0 |
| Look-alikes muted | Laughs, claps, knocks… that got silenced | Near 0 |
| Speech cut | Seconds of talking silenced by mistake, per hour | Near 0 |

A detector plugs in by returning mutes **and the moment it decided**. The
scorer enforces the engine's look-ahead budget, so no detector can peek
further ahead than the real buffer would allow.

See [bench/DATA.md](bench/DATA.md) for data sources and licenses.

## Current scores (dev set, real C++ engine, 100 ms look-ahead)

| Metric | Rules detector |
|---|---|
| Coughs/sneezes caught | 12.8% |
| Leak when caught (median) | 0 ms |
| False mutes per hour | 70.5 |
| Look-alikes muted | 3.3% |
| Speech cut per hour | 24 s |

The engine does its job: when the detector decides in time, nothing leaks.
The rules detector is the weak part. It misses most real coughs and fires on
word onsets. The trained model (piece 5) replaces it in the detector slot.
