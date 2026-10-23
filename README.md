# Hush

Automatic cough and sneeze muting for calls and meetings. Hush listens a
fraction of a second ahead of what it sends, silences just the cough or
sneeze, and unmutes on its own.

> Status: early development. Piece 1 of 6 (the test bench) is done.

## Plan

| # | Piece | Status |
|---|---|---|
| 1 | **Test bench**: labeled test calls, SQLite catalog, scoring | ✅ done |
| 2 | Engine core (C++): look-ahead buffer, detector slot, mute and fade | next |
| 3 | Rules detector in the engine, scored on the bench | |
| 4 | Chrome extension (Meet, Teams web, Zoom web) | |
| 5 | Trained on-device model in the detector slot | |
| 6 | Desktop virtual microphone, then Android demo | |

## The test bench

Every change to Hush is judged on the same labeled test calls, so "better"
always means a number went the right way.

```bash
cd bench
pip install -r requirements.txt
./scripts/fetch_data.sh             # public clips, ~140 MB
python -m hush_bench.build          # 80 dev + 40 test calls -> data/scenes, data/catalog.db
python -m hush_bench.score --detector rules --split dev --lookahead-ms 100
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
