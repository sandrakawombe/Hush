"""Convert extracted Coswara folders to mono 16 kHz WAVs plus an index.

    python prep_coswara.py SRC_DIR DST_DIR
"""
import json
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hush_bench import SR, audio  # noqa: E402

KEEP = ["cough-heavy", "cough-shallow", "counting-normal", "counting-fast",
        "breathing-deep", "breathing-shallow", "vowel-a"]


def one(args):
    person, dst = args
    out = {}
    for k in KEEP:
        p = person / f"{k}.wav"
        if not p.exists() or p.stat().st_size < 2000:
            continue
        try:
            x = audio.read(p)
        except Exception:  # a few files in the dataset are empty or damaged
            continue
        if len(x) < SR * 0.5 or np.abs(x).max() < 1e-3:
            continue
        d = dst / person.name
        d.mkdir(parents=True, exist_ok=True)
        audio.write(d / f"{k}.wav", x / max(1e-3, np.abs(x).max()) * 0.9)
        out[k] = len(x) / SR
    return person.name, out


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    people = sorted(p for day in src.iterdir() if day.is_dir() for p in day.iterdir() if p.is_dir())
    with Pool(2) as pool:
        res = pool.map(one, [(p, dst) for p in people])
    ok = {n: o for n, o in res if "cough-heavy" in o or "cough-shallow" in o}
    dst.mkdir(parents=True, exist_ok=True)
    (dst / "index.json").write_text(json.dumps(ok))
    print(f"{len(people)} people, {len(ok)} with usable coughs -> {dst}")


if __name__ == "__main__":
    main()
