"""Small audio helpers: read/write WAV, resample, levels."""
from math import gcd

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

from . import SR


def read(path, sr=SR):
    """Read a WAV file as mono float32 in [-1, 1] at `sr`."""
    rate, x = wavfile.read(path)
    if x.dtype.kind == "i":
        x = x.astype(np.float32) / np.iinfo(x.dtype).max
    elif x.dtype.kind == "u":
        x = (x.astype(np.float32) - 128) / 128
    x = x.astype(np.float32)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if rate != sr:
        g = gcd(rate, sr)
        x = resample_poly(x, sr // g, rate // g).astype(np.float32)
    return x


def write(path, x, sr=SR):
    y = np.clip(x, -1, 1)
    wavfile.write(path, sr, (y * 32767).astype(np.int16))


def rms_db(x):
    return 10 * np.log10(np.mean(x.astype(np.float64) ** 2) + 1e-12)


def frame_db(x, sr=SR, ms=10):
    """Level in dB of consecutive non-overlapping frames."""
    n = int(sr * ms / 1000)
    k = len(x) // n
    if k == 0:
        return np.array([rms_db(x)])
    f = x[: k * n].reshape(k, n).astype(np.float64)
    return 10 * np.log10((f ** 2).mean(axis=1) + 1e-12)


def active_span(x, sr=SR, below_peak_db=30, ms=10):
    """(start, end) samples of the part of `x` within `below_peak_db` of its loudest frame."""
    db = frame_db(x, sr, ms)
    on = np.where(db > db.max() - below_peak_db)[0]
    n = int(sr * ms / 1000)
    return int(on[0] * n), int((on[-1] + 1) * n)
