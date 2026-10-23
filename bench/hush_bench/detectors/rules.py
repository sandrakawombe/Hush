"""Rules detector: Python port of the browser prototype.

A frame is the start of a cough/sneeze when it is a sharp jump above the
recent floor, louder than the talker's normal voice, and noisy rather than
tonal for several frames in a row.
"""
from dataclasses import asdict, dataclass

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

from . import Detector, Mute

HOP_S = 0.010
WIN = 512


@dataclass
class RulesParams:
    onset_db: float = 14.0       # jump above the trailing 150 ms floor
    loud_db: float = 4.0         # above the running speech level
    flat: float = 0.22           # spectral flatness that counts as "noisy"
    hi_ratio: float = 0.35       # share of energy above 2 kHz that counts as "noisy"
    confirm_frames: int = 4      # frames of evidence before deciding
    hold_s: float = 0.45         # minimum mute length
    pre_roll_s: float = 0.03     # start the mute this long before the onset
    tail_s: float = 0.05         # keep muted this long after it fades


def features(pcm, sr):
    hop = int(sr * HOP_S)
    if len(pcm) < WIN:
        pcm = np.pad(pcm, (0, WIN - len(pcm)))
    frames = sliding_window_view(pcm, WIN)[::hop].astype(np.float64)
    db = 10 * np.log10((frames ** 2).mean(axis=1) + 1e-12)
    spec = np.abs(np.fft.rfft(frames * np.hanning(WIN), axis=1))[:, 2:WIN // 2] ** 2 + 1e-18
    flat = np.exp(np.log(spec).mean(axis=1)) / spec.mean(axis=1)
    hi_bin = int(2000 / (sr / WIN)) - 2
    hi = spec[:, hi_bin:].sum(axis=1) / spec.sum(axis=1)
    t = np.arange(len(db)) * HOP_S
    return t, db, flat, hi


class RulesDetector(Detector):
    name = "rules"

    def __init__(self, **overrides):
        self.p = RulesParams(**overrides)

    def params(self):
        return asdict(self.p)

    def mutes(self, pcm, sr):
        p = self.p
        t, db, flat, hi = features(pcm, sr)
        win_s = WIN / sr
        noisy = (flat > p.flat) | (hi > p.hi_ratio)
        out, speech_level, i = [], -40.0, 15
        n = len(db)
        while i < n:
            floor = db[i - 15 : i - 2].mean()
            if db[i] > -45 and flat[i] < 0.15:
                speech_level = 0.97 * speech_level + 0.03 * db[i]
            if db[i] - floor >= p.onset_db and db[i] >= speech_level + p.loud_db:
                k = min(n, i + p.confirm_frames)
                if noisy[i:k].sum() >= p.confirm_frames - 1:
                    j = i
                    while j < n and db[j] > floor + 6:
                        j += 1
                    start = t[i] - p.pre_roll_s
                    decided = t[k - 1] + win_s
                    end = max(t[min(j, n - 1)], t[i] + p.hold_s) + p.tail_s
                    if out and start <= out[-1].end_s + 0.1:
                        out[-1].end_s = max(out[-1].end_s, end)
                    else:
                        out.append(Mute(start, end, decided, classify(db, flat, i)))
                    i = max(i + 1, j)
                    continue
            i += 1
        return out


def classify(db, flat, i):
    """Sneezes usually have a breathy inhale 150-500 ms before the burst."""
    a = max(0, i - 50)
    inhale = ((flat[a : i - 3] > 0.3) & (db[a : i - 3] > -42)).sum()
    return "sneeze" if inhale >= 12 else "cough"
