"""Check that the C++ rules detector decides exactly like the Python reference.

    python -m hush_bench.parity --split dev

Compares every proposed mute (start, end, decision time, cough/sneeze guess)
on every call. One frame (10 ms) of difference is allowed for rounding.
"""
import argparse

from . import SR, audio, catalog
from .detectors.engine import run_tool
from .detectors.rules import RulesDetector

TOL = 0.0101


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--split", default="dev", choices=["dev", "test"])
    args = ap.parse_args()
    con = catalog.connect()
    clips = catalog.clips(con, args.split)
    paths = [catalog.DB_PATH.parent / c["path"] for c in clips]
    cpp = run_tool("proposals", paths)
    py = RulesDetector()
    total = same = 0
    problems = []
    for p in paths:
        a = py.mutes(audio.read(p), SR)
        b = cpp[p.resolve()]["mutes"]
        total += max(len(a), len(b))
        if len(a) != len(b):
            problems.append(f"{p.name}: python {len(a)} mutes, c++ {len(b)}")
            continue
        for m, (s, e, d, k) in zip(a, b):
            if abs(m.start_s - s) <= TOL and abs(m.end_s - e) <= TOL and abs(m.decided_s - d) <= TOL and m.kind == k:
                same += 1
            else:
                problems.append(f"{p.name}: python ({m.start_s:.3f}, {m.end_s:.3f}, {m.decided_s:.3f}, {m.kind}) "
                                f"vs c++ ({s:.3f}, {e:.3f}, {d:.3f}, {k})")
    print(f"{len(paths)} calls, {total} mutes, {same} identical")
    for line in problems[:20]:
        print("  " + line)
    raise SystemExit(0 if same == total else 1)


if __name__ == "__main__":
    main()
