"""Log-mel features: the model's view of the audio.

Defined in Hz and milliseconds, not samples, so the same code gives nearly
the same features at 16 kHz and 48 kHz. The C++ engine reimplements exactly
this; the bench checks they agree.

  frame k covers samples [k*hop, k*hop + win)   (32 ms window, 10 ms hop)
  power spectrum of the Hann-windowed frame, normalized by the window sum
  40 mel bands, 60–7600 Hz, triangles with unit peak (HTK mel scale)
  feature = log10(band energy + 1e-10)
"""
import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

N_MELS = 40
F_LO, F_HI = 60.0, 7600.0
WIN_S, HOP_S = 0.032, 0.010


def geometry(sr):
    win = int(round(WIN_S * sr))
    hop = int(round(HOP_S * sr))
    nfft = 1
    while nfft < win:
        nfft <<= 1
    return win, hop, nfft


def hz_to_mel(f):
    return 2595.0 * np.log10(1.0 + np.asarray(f, dtype=np.float64) / 700.0)


def mel_to_hz(m):
    return 700.0 * (10 ** (np.asarray(m, dtype=np.float64) / 2595.0) - 1.0)


def mel_bank(sr, nfft):
    """(N_MELS, nfft//2+1) triangular filters with unit peak."""
    edges = mel_to_hz(np.linspace(hz_to_mel(F_LO), hz_to_mel(F_HI), N_MELS + 2))
    freqs = np.arange(nfft // 2 + 1) * sr / nfft
    fb = np.zeros((N_MELS, len(freqs)))
    for m in range(N_MELS):
        lo, mid, hi = edges[m], edges[m + 1], edges[m + 2]
        up = (freqs - lo) / (mid - lo)
        down = (hi - freqs) / (hi - mid)
        fb[m] = np.clip(np.minimum(up, down), 0, None)
    return fb


def logmel(pcm, sr):
    """(frames, N_MELS) float32 features of a whole clip."""
    win, hop, nfft = geometry(sr)
    x = np.asarray(pcm, dtype=np.float32)
    if len(x) < win:
        x = np.pad(x, (0, win - len(x)))
    frames = sliding_window_view(x, win)[::hop].astype(np.float64)
    w = np.hanning(win)
    spec = np.fft.rfft(frames * w, n=nfft, axis=1)
    power = (spec.real ** 2 + spec.imag ** 2) / (w.sum() ** 2)
    mel = power @ mel_bank(sr, nfft).T
    return np.log10(mel + 1e-10).astype(np.float32)
