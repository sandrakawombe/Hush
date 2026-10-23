"""Train the Hush model: a tiny causal network over log-mel frames.

Architecture (chosen so it streams frame by frame and is easy to run in C++
without any ML library):

    40 mel bands ─► conv k=5 (32 ch) ─► conv k=5 dil=2 ─► conv k=5 dil=4 ─► dense ─► p(cough)
                    ReLU                 ReLU             ReLU

It only ever looks at the past: the newest frame plus 28 before it (290 ms).
About 17,000 weights; roughly 17,000 multiply-adds every 10 ms.

    python train/train.py            # writes models/hush_tcn_v2.npz
"""
import argparse
import time
from pathlib import Path

import numpy as np
import torch
from torch import nn

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "train" / "data"
MODELS = ROOT / "models"
CH, K, DILS = 32, 5, (1, 2, 4)
RF = 1 + sum((K - 1) * d for d in DILS)  # receptive field in frames (29); set from args in main()


class TCN(nn.Module):
    def __init__(self, n_mels=40):
        super().__init__()
        layers, c_in = [], n_mels
        for d in DILS:
            layers += [nn.Conv1d(c_in, CH, K, dilation=d), nn.ReLU()]
            c_in = CH
        self.convs = nn.Sequential(*layers)
        self.out = nn.Linear(CH, 1)
        self.drop = nn.Dropout(0.1)

    def forward(self, x):  # x: (batch, mels, RF)
        return self.out(self.drop(self.convs(x)[:, :, -1])).squeeze(1)


def load(split):
    d = np.load(DATA / f"{split}.npz")
    feats, labels, lens = d["feats"].astype(np.float32), d["labels"], d["lens"]
    starts = np.concatenate([[0], np.cumsum(lens)[:-1]])
    # every frame with RF-1 frames of history inside its own call
    idx = np.concatenate([np.arange(s + RF - 1, s + n) for s, n in zip(starts, lens)])
    return feats, labels, idx


def windows(feats, idx, mean, std, shift=None):
    w = feats[idx[:, None] + np.arange(-RF + 1, 1)[None, :]]  # (B, RF, mels)
    if shift is not None:
        w = w + shift[:, None, None]  # level change: +-0.8 in log10 = +-8 dB
    return torch.from_numpy(((w - mean) / std).transpose(0, 2, 1).astype(np.float32))


def auc(scores, labels):
    order = np.argsort(scores)
    ranks = np.empty(len(scores))
    ranks[order] = np.arange(1, len(scores) + 1)
    pos = labels == 1
    return (ranks[pos].sum() - pos.sum() * (pos.sum() + 1) / 2) / (pos.sum() * (~pos).sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=14)
    ap.add_argument("--steps", type=int, default=400)
    ap.add_argument("--batch", type=int, default=512)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="hush_tcn_v2.npz")
    ap.add_argument("--channels", type=int, default=32)
    ap.add_argument("--dils", default="1,2,4")
    args = ap.parse_args()
    global CH, DILS, RF
    CH = args.channels
    DILS = tuple(int(v) for v in args.dils.split(","))
    RF = 1 + sum((K - 1) * d for d in DILS)
    torch.manual_seed(args.seed)
    rng = np.random.default_rng(args.seed)
    torch.set_num_threads(2)

    tf, tl, tidx = load("train")
    vf, vl, vidx = load("val")
    mean = tf.mean(axis=0).astype(np.float32)
    std = (tf.std(axis=0) + 1e-3).astype(np.float32)
    pos, neg = tidx[tl[tidx] == 1], tidx[tl[tidx] == 0]
    vsel = rng.choice(vidx, 60000, replace=False)
    vx, vy = windows(vf, vsel, mean, std), vl[vsel]
    print(f"train frames {len(tidx)} ({len(pos)} cough/sneeze), receptive field {RF} frames")

    model = TCN()
    opt = torch.optim.AdamW(model.parameters(), lr=2e-3, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.epochs * args.steps)
    lossf = nn.BCEWithLogitsLoss()
    best, best_state = 0, None
    for ep in range(args.epochs):
        t0 = time.time()
        model.train()
        total = 0.0
        for _ in range(args.steps):
            npos = int(args.batch * 0.35)
            b = np.concatenate([rng.choice(pos, npos), rng.choice(neg, args.batch - npos)])
            x = windows(tf, b, mean, std, rng.uniform(-0.8, 0.8, len(b)).astype(np.float32))
            y = torch.from_numpy(tl[b].astype(np.float32))
            opt.zero_grad()
            loss = lossf(model(x), y)
            loss.backward()
            opt.step()
            sched.step()
            total += loss.item()
        model.eval()
        with torch.no_grad():
            s = torch.cat([model(vx[i : i + 8192]) for i in range(0, len(vx), 8192)]).numpy()
        a = auc(s, vy)
        if a > best:
            best, best_state = a, {k: v.clone() for k, v in model.state_dict().items()}
        print(f"epoch {ep + 1:2d}  loss {total / args.steps:.4f}  val AUC {a:.4f}  ({time.time() - t0:.0f}s)")

    model.load_state_dict(best_state)
    MODELS.mkdir(exist_ok=True)
    sd = {k: v.numpy() for k, v in model.state_dict().items()}
    convs = [sd[f"convs.{2 * i}.weight"] for i in range(len(DILS))]
    biases = [sd[f"convs.{2 * i}.bias"] for i in range(len(DILS))]
    arrays = {f"w{i}": w for i, w in enumerate(convs)} | {f"b{i}": b for i, b in enumerate(biases)}
    np.savez(MODELS / args.out, mean=mean, std=std, dils=np.array(DILS), k=np.array(K),
             wo=sd["out.weight"], bo=sd["out.bias"], **arrays)
    n = sum(v.size for k, v in sd.items())
    print(f"saved models/{args.out}: {n} weights, best val AUC {best:.4f}")


if __name__ == "__main__":
    main()
