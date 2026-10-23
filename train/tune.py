"""Pick the decision-rule settings on the VALIDATION calls only.

Tries a grid of settings at the engine's look-ahead budget and prints the
trade-off between catching coughs and muting by mistake. Never touches the
bench's dev or test calls.

    python train/tune.py
"""
import itertools
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bench"))
from hush_bench import SR, score  # noqa: E402
from hush_bench.detectors.model import ModelDetector, ModelParams, decide  # noqa: E402

LOOKAHEAD = 0.100
MODEL_NAME = sys.argv[1] if len(sys.argv) > 1 else "hush_tcn_v2.npz"


def main():
    data = ROOT / "train" / "data"
    pcm = np.load(data / "val_audio.npz")["pcm"].astype(np.float32) / 32767
    metas = json.loads((data / "val_meta.json").read_text())
    det = ModelDetector(model=ROOT / "models" / MODEL_NAME)
    probs = [det.probs(x, SR) for x in pcm]
    dur = pcm.shape[1] / SR

    def evaluate(prm):
        rows, flags, cut, stray = [], [], 0.0, 0
        for p, m in zip(probs, metas):
            spans = score.apply_budget(decide(p, SR, prm), LOOKAHEAD, dur)
            r, f, c = score.score_clip(m["events"], m["speech"], spans)
            rows += r
            flags += f
            cut += c
            others = [e for e in m["events"] if not e["is_target"]]
            # false mutes that touched no event at all: talking or silence
            stray += sum(1 for a, b, bad in f if bad and not any(score.overlap(a, b, e["onset_s"], e["offset_s"]) > 0 for e in others))
        out = score.summarize(rows, flags, cut, dur * len(pcm))
        out["stray_per_hour"] = round(stray / (dur * len(pcm) / 3600), 1)
        return out

    grid = itertools.product([0.6, 0.7, 0.8, 0.9, 0.95, 0.98], [2, 3, 4], [0.04, 0.06, 0.08], [0.3, 0.5], [3, 6])
    results = []
    for on_th, on_frames, lead, off_th, off_frames in grid:
        prm = ModelParams(on_th=on_th, on_frames=on_frames, lead_s=lead, off_th=off_th, off_frames=off_frames)
        results.append((prm, evaluate(prm)))

    print(f"{len(results)} settings on {len(pcm)} validation calls ({dur * len(pcm) / 3600:.2f} h)")
    print("These calls are packed with look-alike sounds, so judge mistakes by rate:")
    print("  look-alikes muted = share of laughs, claps, dogs... that got silenced")
    print("  stray mutes/h     = mutes on plain talking or silence\n")
    print(f"{'look-alike cap':>14} {'caught':>7} {'leak p90':>9} {'look-alikes':>11} {'stray/h':>8} {'speech cut s/h':>15}  settings")
    chosen = None
    for cap in (5, 10, 15, 25):
        ok = [(p, m) for p, m in results if m["lookalike_muted_pct"] <= cap and m["stray_per_hour"] <= 10]
        if not ok:
            continue
        p, m = max(ok, key=lambda r: (r[1]["caught_pct"], -r[1]["lookalike_muted_pct"]))
        print(f"{cap:>13}% {m['caught_pct']:>6}% {m['leak_ms_p90']!s:>8} {m['lookalike_muted_pct']:>10}% {m['stray_per_hour']:>8} "
              f"{m['speech_cut_s_per_hour']:>15}  on {p.on_th}x{p.on_frames} lead {p.lead_s} off {p.off_th}x{p.off_frames}")
        if cap == 10:
            chosen = p
    if chosen:
        out = ROOT / "models" / (MODEL_NAME.replace(".npz", ".decision.json"))
        out.write_text(json.dumps(chosen.__dict__, indent=2))
        print(f"\nsaved the 10% setting to {out.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
