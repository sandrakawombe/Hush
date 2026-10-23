"""Score a detector on the labeled test calls and save the run in the catalog.

    python -m hush_bench.score --detector rules --split dev
    python -m hush_bench.score --detector rules --lookahead-ms 100 --set onset_db=12

What counts:
  caught         a cough/sneeze whose first 300 ms (the loud attack) is >= 80% muted
  leak           how late the mute started after the true onset (0 = nothing escaped)
  false mute     a mute that touches no cough or sneeze
  look-alike hit a laugh, clap, knock... with >= 30% of its attack muted
  speech cut     seconds of talking silenced by false mutes

Look-ahead budget: the engine delays outgoing audio by this much, so a mute
can begin at most this long before the moment the detector decided. Any
earlier start the detector asks for is pushed later, and the gap leaks.
"""
import argparse
import json
from collections import defaultdict

import numpy as np

from . import SR, audio, catalog, detectors

ATTACK_S = 0.30
CAUGHT_AT = 0.80
LOOKALIKE_AT = 0.30
FALSE_MARGIN_S = 0.05


def overlap(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def merge(spans):
    out = []
    for a, b in sorted(spans):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return [tuple(x) for x in out]


def apply_budget(mutes, lookahead_s, duration_s):
    """Turn detector proposals into what a live engine could actually do."""
    spans = []
    for m in mutes:
        start = max(m.start_s, m.decided_s - lookahead_s, 0.0)
        end = min(m.end_s, duration_s)
        if end > start:
            spans.append((start, end))
    return merge(spans)


def score_clip(events, speech, spans):
    """Per-event coverage/leak and per-mute false flags for one clip."""
    per_event = []
    for e in events:
        a0, a1 = e["onset_s"], min(e["offset_s"], e["onset_s"] + ATTACK_S)
        covered = sum(overlap(a0, a1, s, t) for s, t in spans)
        cov = covered / (a1 - a0) if a1 > a0 else 0.0
        touching = [s for s, t in spans if overlap(a0, a1, s, t) > 0]
        leak = max(0.0, min(touching) - a0) * 1000 if touching else None
        per_event.append((e, cov, leak))
    targets = [e for e in events if e["is_target"]]
    flags = []
    for s, t in spans:
        hit = any(overlap(s, t, e["onset_s"] - FALSE_MARGIN_S, e["offset_s"] + FALSE_MARGIN_S) > 0 for e in targets)
        flags.append((s, t, not hit))
    speech_cut = sum(overlap(s, t, a, b) for s, t, f in flags if f for a, b in speech)
    return per_event, flags, speech_cut


def summarize(rows, flags_all, speech_cut, total_s):
    hours = total_s / 3600
    tg = [(e, c, l) for e, c, l in rows if e["is_target"]]
    la = [(e, c, l) for e, c, l in rows if not e["is_target"]]
    caught = [c >= CAUGHT_AT for _, c, _ in tg]
    leaks = [l for _, c, l in tg if l is not None and c >= CAUGHT_AT]

    def rate(sel):
        return round(100 * float(np.mean(sel)), 1) if len(sel) else None

    by_kind = defaultdict(list)
    for e, c, _ in tg:
        by_kind[e["kind"]].append(c >= CAUGHT_AT)
    la_kind = defaultdict(list)
    for e, c, _ in la:
        la_kind[e["kind"]].append(c >= LOOKALIKE_AT)
    n_false = sum(1 for *_, f in flags_all if f)
    return {
        "calls_hours": round(hours, 3),
        "targets": len(tg),
        "caught_pct": rate(caught),
        "caught_pct_by_kind": {k: rate(v) for k, v in sorted(by_kind.items())},
        "caught_pct_in_pause": rate([c >= CAUGHT_AT for e, c, _ in tg if not e["over_speech"]]),
        "caught_pct_over_speech": rate([c >= CAUGHT_AT for e, c, _ in tg if e["over_speech"]]),
        "leak_ms_median": round(float(np.median(leaks)), 1) if leaks else None,
        "leak_ms_p90": round(float(np.percentile(leaks, 90)), 1) if leaks else None,
        "false_mutes": n_false,
        "false_mutes_per_hour": round(n_false / hours, 1),
        "lookalikes": len(la),
        "lookalike_muted_pct": rate([c >= LOOKALIKE_AT for _, c, _ in la]),
        "lookalike_muted_pct_by_kind": {k: rate(v) for k, v in sorted(la_kind.items())},
        "speech_cut_s_per_hour": round(speech_cut / hours, 1),
    }


def run(detector_name, split, lookahead_ms, overrides, save=True, con=None):
    con = con or catalog.connect()
    det = detectors.get(detector_name, **overrides)
    rows, flags_all, mutes_db, per_event_db = [], [], [], []
    speech_cut, total_s = 0.0, 0.0
    clip_rows = catalog.clips(con, split)
    batch = None
    if getattr(det, "batch", False):
        batch = det.mutes_batch([catalog.DB_PATH.parent / c["path"] for c in clip_rows], lookahead_ms)
    for clip in clip_rows:
        path = catalog.DB_PATH.parent / clip["path"]
        events, speech = catalog.clip_labels(con, clip["id"])
        proposals = batch[path] if batch is not None else det.mutes(audio.read(path), SR)
        spans = apply_budget(proposals, lookahead_ms / 1000, clip["duration_s"])
        per_event, flags, cut = score_clip(events, speech, spans)
        rows += per_event
        flags_all += flags
        speech_cut += cut
        total_s += clip["duration_s"]
        per_event_db += [(e["id"], c, l) for e, c, l in per_event]
        mutes_db += [(clip["id"], s, t, int(f)) for s, t, f in flags]
    if total_s == 0:
        raise SystemExit(f"No calls in split '{split}'. Run: python -m hush_bench.build")
    metrics = summarize(rows, flags_all, speech_cut, total_s)
    params = {"lookahead_ms": lookahead_ms, **det.params()}
    rid = catalog.save_run(con, det.name, split, params, metrics, per_event_db, mutes_db) if save else None
    return rid, metrics


def parse_set(items):
    out = {}
    for it in items or []:
        k, v = it.split("=", 1)
        out[k] = int(v) if v.lstrip("-").isdigit() else float(v)
    return out


def print_report(name, split, lookahead_ms, m, rid=None):
    head = f"{name} on {split}, {lookahead_ms} ms look-ahead" + (f"  (run #{rid})" if rid else "")
    print(head)
    print("-" * len(head))
    print(f"  coughs/sneezes caught   {m['caught_pct']}%  of {m['targets']}   {m['caught_pct_by_kind']}")
    print(f"    in pauses / over talk {m['caught_pct_in_pause']}% / {m['caught_pct_over_speech']}%")
    print(f"  leak when caught        median {m['leak_ms_median']} ms, p90 {m['leak_ms_p90']} ms")
    print(f"  false mutes             {m['false_mutes']}  ({m['false_mutes_per_hour']} per hour)")
    print(f"  look-alikes muted       {m['lookalike_muted_pct']}%  of {m['lookalikes']}   {m['lookalike_muted_pct_by_kind']}")
    print(f"  speech cut              {m['speech_cut_s_per_hour']} s per hour of calls")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--detector", default="rules")
    ap.add_argument("--split", default="dev", choices=["dev", "test"])
    ap.add_argument("--lookahead-ms", type=float, default=40)
    ap.add_argument("--set", nargs="*", help="detector parameter overrides, e.g. onset_db=12")
    ap.add_argument("--no-save", action="store_true")
    args = ap.parse_args()
    rid, m = run(args.detector, args.split, args.lookahead_ms, parse_set(args.set), save=not args.no_save)
    print_report(args.detector, args.split, args.lookahead_ms, m, rid)


if __name__ == "__main__":
    main()
