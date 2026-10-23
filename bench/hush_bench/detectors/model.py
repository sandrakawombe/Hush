"""Model detector: the trained network plus a small decision rule.

The network gives p(cough or sneeze) every 10 ms from the past 290 ms of
audio. The decision rule turns that into mutes:

  start  when p >= on_th for on_frames frames in a row
         the mute begins `lead_s` before the end of the first of those frames
  stop   when p < off_th for off_frames frames in a row, plus tail_s
"""
import json
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

from ..features import geometry, logmel
from . import Detector, Mute

MODEL = Path(__file__).resolve().parents[3] / "models" / "hush_tcn_v2.npz"


@dataclass
class ModelParams:
    on_th: float = 0.8
    on_frames: int = 3
    off_th: float = 0.4
    off_frames: int = 5
    lead_s: float = 0.06
    tail_s: float = 0.05
    min_hold_s: float = 0.15
    merge_s: float = 0.10


class Net:
    """NumPy inference for the TCN in train/train.py."""

    def __init__(self, path=MODEL):
        d = np.load(path)
        self.mean, self.std = d["mean"], d["std"]
        self.k = int(d["k"])
        self.dils = [int(v) for v in d["dils"]]
        self.layers = [(d[f"w{i}"], d[f"b{i}"]) for i in range(len(self.dils))]
        self.wo, self.bo = d["wo"], d["bo"]
        self.rf = 1 + sum((self.k - 1) * dd for dd in self.dils)

    def probs(self, feats):
        """p for every frame; frames without a full 290 ms of history get 0."""
        x = ((feats - self.mean) / self.std).astype(np.float32)  # (T, mels)
        for (w, b), dil in zip(self.layers, self.dils):
            span = (self.k - 1) * dil + 1
            if len(x) < span:
                return np.zeros(len(feats), np.float32)
            win = sliding_window_view(x, span, axis=0)[:, :, ::dil]  # (T', c_in, k)
            x = np.maximum(np.einsum("tck,ock->to", win, w) + b, 0)
        z = x @ self.wo[0] + self.bo[0]
        p = np.zeros(len(feats), np.float32)
        p[self.rf - 1 :] = 1 / (1 + np.exp(-z))
        return p


def decide(p, sr, prm):
    """Causal decision rule over per-frame probabilities -> Mute list."""
    win, hop, _ = geometry(sr)
    out, run, active, below = [], 0, False, 0
    for k, pk in enumerate(p):
        frame_end = (k * hop + win) / sr
        if not active:
            run = run + 1 if pk >= prm.on_th else 0
            if run >= prm.on_frames:
                k0 = k - prm.on_frames + 1
                start = (k0 * hop + win) / sr - prm.lead_s
                if out and start <= out[-1].end_s + prm.merge_s:
                    start = min(start, out[-1].end_s)
                out.append(Mute(start, None, frame_end, "cough"))
                active, below, run = True, 0, 0
        else:
            below = below + 1 if pk < prm.off_th else 0
            if below >= prm.off_frames:
                last_on = k - prm.off_frames
                end = max((last_on * hop + win) / sr + prm.tail_s, out[-1].start_s + prm.min_hold_s)
                out[-1].end_s = end
                active = False
    if active:
        out[-1].end_s = (len(p) * hop + win) / sr
    # merge mutes that ended up touching
    merged = []
    for m in out:
        if merged and m.start_s <= merged[-1].end_s:
            merged[-1].end_s = max(merged[-1].end_s, m.end_s)
        else:
            merged.append(m)
    return merged


class ModelDetector(Detector):
    name = "model"

    def __init__(self, model=None, **overrides):
        path = Path(model) if model else MODEL
        if not path.is_absolute() and not path.exists():
            path = MODEL.parent / path
        self.net = Net(path)
        self.model_path = str(path)
        # start from the settings tuned on validation calls (train/tune.py), if saved
        tuned = path.with_suffix(".decision.json")
        base = json.loads(tuned.read_text()) if tuned.exists() else {}
        self.p = ModelParams(**{**base, **overrides})

    def params(self):
        return {"model": Path(self.model_path).name, **asdict(self.p)}

    def probs(self, pcm, sr):
        return self.net.probs(logmel(pcm, sr))

    def mutes(self, pcm, sr):
        return decide(self.probs(pcm, sr), sr, self.p)
