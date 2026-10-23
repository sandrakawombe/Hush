"""Write a few synthetic test calls (talking + bursts) for CI parity checks."""
import sys
import numpy as np
from scipy.io import wavfile

out = sys.argv[1]
for k, sr in enumerate((16000, 48000, 44100)):
    rng = np.random.default_rng(k)
    t = np.arange(int(sr * 6)) / sr
    voice = 0.05 * (0.55 + 0.45 * np.sin(2 * np.pi * 4.2 * t) ** 2) * sum(np.sin(2 * np.pi * 140 * h * t) / h for h in range(1, 9))
    x = voice + 0.001 * rng.standard_normal(len(t))
    for b in (2.0, 4.1):
        i = int(b * sr)
        n = int(0.3 * sr)
        u = np.arange(n) / sr
        x[i:i + n] += 0.6 * np.minimum(1, u / 0.004) * np.exp(-u / 0.07) * rng.standard_normal(n)
    wavfile.write(f"{out}/synthetic_{sr}.wav", sr, (np.clip(x, -1, 1) * 32767).astype(np.int16))
