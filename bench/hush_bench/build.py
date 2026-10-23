"""Build labeled test calls from public clips and record them in the catalog.

Each test call is ~30 s of one person talking (spoken digits), with coughs and
sneezes mixed in at known times, plus look-alike sounds that must NOT be muted
(laughing, clapping, knocking, typing, breathing, mouse clicks). Some events
land in pauses, some over speech. Half the calls get background noise.

The dev and test sets never share a speaker or a source clip, so a model tuned
on dev is judged fairly on test.

    python -m hush_bench.build            # 80 dev + 40 test calls
"""
import argparse
import csv
from pathlib import Path

import numpy as np

from . import SR, audio, catalog

DATA = Path(__file__).resolve().parent.parent / "data"
RAW = DATA / "raw"

TARGETS = ["coughing", "sneezing"]
# look-alikes and how loud they are relative to the talker's voice (dB range)
LOOKALIKES = {
    "laughing": (0, 10), "clapping": (2, 12), "door_wood_knock": (2, 12),
    "keyboard_typing": (-6, 4), "breathing": (-10, 0), "mouse_click": (-6, 4),
}
TARGET_DB = (4, 14)
SPEAKERS = {"dev": ["jackson", "nicolas", "theo", "yweweler"], "test": ["george", "lucas"]}
FOLDS = {"dev": {1, 2, 3, 4}, "test": {5}}
SPEECH_DB = -24.0


def load_esc50():
    by = {}
    with open(RAW / "esc50" / "esc50.csv") as f:
        for r in csv.DictReader(f):
            if r["category"] in TARGETS or r["category"] in LOOKALIKES:
                split = "test" if int(r["fold"]) in FOLDS["test"] else "dev"
                by.setdefault((split, r["category"]), []).append(r["filename"])
    return by


def load_event(filename):
    x = audio.read(RAW / "esc50" / filename)
    a, b = audio.active_span(x, below_peak_db=25)
    return x[a : min(b, a + 2 * SR)]


def load_digit(filename):
    x = audio.read(RAW / "fsdd" / filename)
    a, b = audio.active_span(x, below_peak_db=30)
    x = x[a:b]
    return x * 10 ** ((SPEECH_DB - audio.rms_db(x)) / 20)


def noise(n, rng, level_db):
    """Pink-ish noise (1/f) at the given RMS level."""
    w = rng.standard_normal(n)
    spec = np.fft.rfft(w)
    f = np.arange(len(spec))
    spec[1:] /= np.sqrt(f[1:])
    y = np.fft.irfft(spec, n)
    return (y / (np.sqrt(np.mean(y ** 2)) + 1e-12) * 10 ** (level_db / 20)).astype(np.float32)


def make_call(rng, split, esc, digits, dur=30.0):
    n = int(dur * SR)
    voice = np.zeros(n, np.float32)
    speech, utter = [], []
    t = rng.uniform(0.4, 1.0)
    while t < dur - 3:
        u0 = t
        for _ in range(rng.integers(3, 9)):
            d = load_digit(digits[rng.integers(len(digits))])
            s = int(t * SR)
            if s + len(d) >= n:
                break
            voice[s : s + len(d)] += d
            speech.append((t, t + len(d) / SR))
            t += len(d) / SR + rng.uniform(0.03, 0.15)
        utter.append((u0, t))
        t += rng.uniform(0.8, 3.0)

    mix = voice.copy()
    events, taken = [], []
    plan = [(k, True) for k in rng.choice(TARGETS, rng.integers(2, 4))]
    plan += [(k, False) for k in rng.choice(list(LOOKALIKES), rng.integers(1, 3), replace=False)]
    rng.shuffle(plan)
    for kind, is_target in plan:
        src = esc[(split, kind)][rng.integers(len(esc[(split, kind)]))]
        ev = load_event(src)
        lo, hi = TARGET_DB if is_target else LOOKALIKES[kind]
        peak = audio.frame_db(ev).max()
        ev = ev * 10 ** ((SPEECH_DB + rng.uniform(lo, hi) - peak) / 20)
        L = len(ev) / SR
        for _ in range(50):  # find a spot that doesn't collide with another event
            if rng.random() < 0.6:  # in a pause between utterances
                gaps = [(utter[i][1], utter[i + 1][0]) for i in range(len(utter) - 1)]
                gaps = [g for g in gaps if g[1] - g[0] > L + 0.2]
                if not gaps:
                    continue
                g = gaps[rng.integers(len(gaps))]
                start = g[0] + rng.uniform(0.05, max(0.06, g[1] - g[0] - L - 0.1))
            else:  # while talking
                u = utter[rng.integers(len(utter))]
                start = rng.uniform(u[0], max(u[0] + 0.01, u[1] - 0.2))
            if start + L < dur - 0.2 and all(start > b + 0.4 or start + L < a - 0.4 for a, b in taken):
                break
        else:
            continue
        s = int(start * SR)
        mix[s : s + len(ev)] += ev
        db = audio.frame_db(ev)
        on = np.where(db > db.max() - 20)[0]
        off = np.where(db > db.max() - 30)[0]
        onset, offset = start + on[0] * 0.01, start + (off[-1] + 1) * 0.01
        taken.append((onset, offset))
        events.append({
            "kind": kind, "is_target": is_target, "onset_s": round(onset, 3), "offset_s": round(offset, 3),
            "over_speech": any(a < offset and b > onset for a, b in speech), "source": f"ESC-50/{src}",
        })

    noise_db = None
    if rng.random() < 0.5:
        noise_db = float(rng.uniform(-55, -40))
        mix += noise(n, rng, noise_db)
    else:
        mix += noise(n, rng, -75)
    return mix, events, speech, noise_db


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", type=int, default=80)
    ap.add_argument("--test", type=int, default=40)
    ap.add_argument("--seed", type=int, default=2026)
    args = ap.parse_args()

    esc = load_esc50()
    fsdd = sorted(p.name for p in (RAW / "fsdd").glob("*.wav"))
    con = catalog.connect()
    catalog.reset_clips(con)
    rng = np.random.default_rng(args.seed)
    for split, count in (("dev", args.dev), ("test", args.test)):
        out = DATA / "scenes" / split
        out.mkdir(parents=True, exist_ok=True)
        for i in range(count):
            spk = SPEAKERS[split][i % len(SPEAKERS[split])]
            digits = [f for f in fsdd if f.split("_")[1] == spk]
            mix, events, speech, noise_db = make_call(rng, split, esc, digits)
            name = f"{split}-{i:03d}"
            audio.write(out / f"{name}.wav", mix)
            catalog.add_clip(con, name, split, f"scenes/{split}/{name}.wav", len(mix) / SR, spk, noise_db, events, speech)
        con.commit()
    for split in ("dev", "test"):
        r = con.execute(
            "SELECT COUNT(DISTINCT c.id) n, SUM(e.is_target) t, SUM(1 - e.is_target) l, SUM(e.over_speech) o "
            "FROM clips c JOIN events e ON e.clip_id = c.id WHERE c.split = ?", (split,)).fetchone()
        print(f"{split}: {r['n']} calls, {r['t']} coughs/sneezes, {r['l']} look-alikes, {r['o']} events over speech")


if __name__ == "__main__":
    main()
