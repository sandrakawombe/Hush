"""Generate training and validation calls for the Hush model.

Each call is 10 s: one talker, 0–3 coughs/sneezes, 0–3 other sounds (laughs,
claps, typing, dogs, doors, breathing, sustained "aaah"...), background noise,
random levels and pitch. Every 10 ms frame gets a label: 1 if a cough or
sneeze is sounding in that frame's window.

Sources:
  Coswara (CC BY 4.0): coughs, counting out loud, breathing, vowels from 227 people
  ESC-50 (CC BY-NC):   coughs, sneezes and everyday sounds
  FSDD (CC BY-SA):     spoken digits

Splits never share a person or a source clip, and the bench's locked test set
(ESC-50 fold 5, speakers george and lucas) is never used here:

    train    ESC-50 folds 1–3, FSDD jackson/nicolas/theo, 80% of Coswara people
    val      ESC-50 fold 4,    FSDD yweweler,             10% of Coswara people
    (the last 10% of Coswara people are held out for a future realistic test)

    python train/gen_data.py --train 3000 --val 300
"""
import argparse
import csv
import functools
import json
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bench"))
from hush_bench import SR, audio  # noqa: E402
from hush_bench.features import geometry, logmel  # noqa: E402

RAW = ROOT / "bench" / "data" / "raw"
OUT = ROOT / "train" / "data"
TARGETS = {"coughing", "sneezing"}
SPLITS = {
    "train": {"folds": {1, 2, 3}, "speakers": ["jackson", "nicolas", "theo"]},
    "val": {"folds": {4}, "speakers": ["yweweler"]},
}
DUR = 10.0


def coswara_people(split):
    """Deterministic person split: 8 of every 10 train, 1 val, 1 held out."""
    import json as _json
    idx = sorted(_json.loads((RAW / "coswara" / "index.json").read_text()))
    want = {"train": set(range(8)), "val": {8}, "holdout": {9}}[split]
    return [p for i, p in enumerate(idx) if i % 10 in want]


def segments(x, below_peak_db=30, gap_s=0.12, min_s=0.08, max_s=1.2):
    """Split a recording of several coughs into individual ones."""
    db = audio.frame_db(x)
    on = db > max(db.max() - below_peak_db, -55)
    out, i, n = [], 0, int(SR * 0.01)
    while i < len(on):
        if not on[i]:
            i += 1
            continue
        j, last = i, i
        while j < len(on) and (on[j] or (j - last) * 0.01 < gap_s):
            if on[j]:
                last = j
            j += 1
        a, b = max(0, i * n - int(0.02 * SR)), min(len(x), (last + 1) * n + int(0.03 * SR))
        if min_s <= (b - a) / SR <= max_s:
            out.append((a, b))
        i = j
    return out


@functools.lru_cache(maxsize=None)
def coswara_file(person, kind):
    p = RAW / "coswara" / person / f"{kind}.wav"
    return audio.read(p) if p.exists() else None


def coswara_pools(split):
    coughs, others, talk = [], [], []
    for person in coswara_people(split):
        for kind in ("cough-heavy", "cough-shallow"):
            x = coswara_file(person, kind)
            if x is not None:
                coughs += [(person, kind, a, b) for a, b in segments(x)]
        for kind in ("breathing-deep", "breathing-shallow", "vowel-a"):
            x = coswara_file(person, kind)
            if x is not None:
                others += [(person, kind, a, b) for a, b in segments(x, below_peak_db=25, gap_s=0.3, min_s=0.3, max_s=2.0)]
        for kind in ("counting-normal", "counting-fast"):
            if coswara_file(person, kind) is not None:
                talk.append((person, kind))
    return coughs, others, talk


def pools(split):
    folds = SPLITS[split]["folds"]
    tg, ng = [], []
    have = {p.name for p in (RAW / "esc50").glob("*.wav")}
    with open(RAW / "esc50" / "esc50.csv") as f:
        for r in csv.DictReader(f):
            if int(r["fold"]) in folds and r["filename"] in have:
                (tg if r["category"] in TARGETS else ng).append((r["filename"], r["category"]))
    digits = {s: sorted(p.name for p in (RAW / "fsdd").glob(f"*_{s}_*.wav")) for s in SPLITS[split]["speakers"]}
    return tg, ng, digits


@functools.lru_cache(maxsize=None)
def event_clip(name):
    x = audio.read(RAW / "esc50" / name)
    a, b = audio.active_span(x, below_peak_db=25)
    return x[a:b]


@functools.lru_cache(maxsize=None)
def digit_clip(name):
    x = audio.read(RAW / "fsdd" / name)
    a, b = audio.active_span(x, below_peak_db=30)
    x = x[a:b]
    return x / (np.sqrt(np.mean(x ** 2)) + 1e-9)


def pink(n, rng):
    spec = np.fft.rfft(rng.standard_normal(n))
    spec[1:] /= np.sqrt(np.arange(1, len(spec)))
    y = np.fft.irfft(spec, n)
    return y / (np.sqrt(np.mean(y ** 2)) + 1e-12)


def active_mask(ev):
    """Samples of `ev` within 25 dB of its loudest 10 ms block."""
    db = audio.frame_db(ev)
    on = np.repeat(db > db.max() - 25, int(SR * 0.01))
    m = np.zeros(len(ev), bool)
    m[: len(on)] = on[: len(ev)]
    return m


def counting_utterance(rng, talk):
    person, kind = talk[rng.integers(len(talk))]
    x = coswara_file(person, kind)
    a, b = audio.active_span(x, below_peak_db=35)
    x = x[a:b]
    L = int(SR * rng.uniform(1.5, 5.0))
    if len(x) > L:
        s0 = rng.integers(0, len(x) - L)
        x = x[s0 : s0 + L]
    db = audio.frame_db(x)
    loud = db > db.max() - 30
    rms = np.sqrt(np.mean(x[: len(loud) * 160].reshape(-1, 160)[loud] ** 2)) if loud.any() else 1.0
    return x / (rms + 1e-9)


def pick_event(rng, tg, ng, cw, is_t):
    """A target (cough/sneeze) or another sound, from ESC-50 or Coswara."""
    coughs, others, _ = cw
    if is_t:
        if coughs and rng.random() < 0.5:
            person, kind, a, b = coughs[rng.integers(len(coughs))]
            return coswara_file(person, kind)[a:b], "coughing"
        name, kind = tg[rng.integers(len(tg))]
        return event_clip(name), kind
    if others and rng.random() < 0.3:
        person, kind, a, b = others[rng.integers(len(others))]
        return coswara_file(person, kind)[a:b], kind
    name, kind = ng[rng.integers(len(ng))]
    return event_clip(name), kind


def make_call(rng, tg, ng, digits, cw):
    n = int(DUR * SR)
    x = np.zeros(n, np.float32)
    target = np.zeros(n, bool)
    speech_db = rng.uniform(-30, -18)
    speech = []
    talk = cw[2]
    if talk and rng.random() < 0.65:  # full-quality counting from Coswara
        t = rng.uniform(0, 1.0)
        while t < DUR - 0.5:
            u = counting_utterance(rng, talk) * 10 ** (speech_db / 20)
            s = int(t * SR)
            e = min(n, s + len(u))
            x[s:e] += u[: e - s]
            speech.append((t, e / SR))
            t = e / SR + rng.uniform(0.3, 2.5)
    elif rng.random() < 0.85:
        spk = list(digits)[rng.integers(len(digits))]
        t = rng.uniform(0, 1.0)
        while t < DUR - 0.5:
            for _ in range(rng.integers(2, 9)):
                d = digit_clip(digits[spk][rng.integers(len(digits[spk]))]) * 10 ** (speech_db / 20)
                s = int(t * SR)
                if s + len(d) >= n:
                    break
                x[s : s + len(d)] += d
                speech.append((t, t + len(d) / SR))
                t += len(d) / SR + rng.uniform(0.02, 0.15)
            t += rng.uniform(0.3, 2.5)

    events, taken = [], []
    plan = [(tg, True)] * int(rng.choice([0, 1, 1, 2, 2, 3])) + [(ng, False)] * int(rng.choice([0, 1, 2, 3]))
    for pool, is_t in plan:
        ev, kind = pick_event(rng, tg, ng, cw, is_t)
        if len(ev) > 2 * SR:
            a = rng.integers(0, len(ev) - 2 * SR)
            ev = ev[a : a + 2 * SR]
        if rng.random() < 0.4:  # pitch/speed change
            up = int(rng.integers(85, 116))
            ev = resample_poly(ev, up, 100).astype(np.float32)
        lo, hi = (-6, 16) if is_t else (-12, 14)
        ev = ev * 10 ** ((speech_db + rng.uniform(lo, hi) - audio.frame_db(ev).max()) / 20)
        L = len(ev) / SR
        for _ in range(30):
            start = rng.uniform(0, max(0.01, DUR - L - 0.05))
            if all(start > b + 0.3 or start + L < a - 0.3 for a, b in taken):
                break
        else:
            continue
        s = int(start * SR)
        e = min(n, s + len(ev))
        x[s:e] += ev[: e - s]
        mask = active_mask(ev)[: e - s]
        if is_t:
            target[s:e] |= mask
        on = np.where(mask)[0]
        if len(on):
            events.append({"kind": kind, "is_target": is_t, "onset_s": (s + on[0]) / SR, "offset_s": (s + on[-1] + 1) / SR,
                           "over_speech": any(a < (s + on[-1]) / SR and b > (s + on[0]) / SR for a, b in speech)})
        taken.append((start, start + L))

    x += (pink(n, rng) * 10 ** (rng.uniform(-65, -35) / 20) if rng.random() < 0.6 else pink(n, rng) * 10 ** (-80 / 20)).astype(np.float32)
    x *= 10 ** (rng.uniform(-12, 6) / 20)
    np.clip(x, -1, 1, out=x)
    return x, target, events, speech


def frame_labels(target):
    """1 if at least 10 ms of cough/sneeze is inside the frame's 32 ms window."""
    win, hop, _ = geometry(SR)
    c = np.concatenate([[0], np.cumsum(target)])
    k = np.arange((len(target) - win) // hop + 1)
    return ((c[k * hop + win] - c[k * hop]) >= int(0.010 * SR)).astype(np.uint8)


def worker(args):
    split, seed, count = args
    rng = np.random.default_rng(seed)
    tg, ng, digits = pools(split)
    cw = coswara_pools(split)
    feats, labels, metas, pcms = [], [], [], []
    for _ in range(count):
        x, target, events, speech = make_call(rng, tg, ng, digits, cw)
        feats.append(logmel(x, SR).astype(np.float16))
        labels.append(frame_labels(target))
        metas.append({"events": events, "speech": speech})
        if split == "val":
            pcms.append((x * 32767).astype(np.int16))
    return feats, labels, metas, pcms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train", type=int, default=3000)
    ap.add_argument("--val", type=int, default=300)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    for split, count in (("train", args.train), ("val", args.val)):
        jobs = [(split, args.seed * 1000 + i + (0 if split == "train" else 500), count // 20) for i in range(20)]
        with Pool(2) as p:
            parts = p.map(worker, jobs)
        feats = [f for part in parts for f in part[0]]
        labels = [l for part in parts for l in part[1]]
        metas = [m for part in parts for m in part[2]]
        lens = np.array([len(f) for f in feats])
        np.savez(OUT / f"{split}.npz", feats=np.concatenate(feats), labels=np.concatenate(labels), lens=lens)
        if split == "val":
            np.savez(OUT / "val_audio.npz", pcm=np.stack([p for part in parts for p in part[3]]))
            (OUT / "val_meta.json").write_text(json.dumps(metas))
        pos = np.concatenate(labels).mean()
        n_ev = sum(sum(e["is_target"] for e in m["events"]) for m in metas)
        print(f"{split}: {len(feats)} calls, {lens.sum()} frames, {100 * pos:.1f}% cough/sneeze frames, {n_ev} coughs/sneezes")


if __name__ == "__main__":
    main()
