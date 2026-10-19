"""Prints level, dominant frequency and the share of two tones per 250 ms, to check a file's pitch and timing.

Usage: python segments.py <recording.wav> <tone-hz> [<tone-hz> ...]
"""
import sys
import wave

import numpy as np

with wave.open(sys.argv[1]) as w:
    rate = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float) / 32768
tones = [float(t) for t in sys.argv[2:]]
x -= x.mean()
n = rate // 4
for k in range(0, len(x) - n, n):
    seg = x[k:k + n]
    m = np.abs(np.fft.rfft(seg * np.hanning(n))) ** 2
    f = np.fft.rfftfreq(n, 1 / rate)
    rms = 20 * np.log10(np.sqrt(np.mean(seg ** 2)) + 1e-12)
    shares = "  ".join(f"{t:.0f}:{100 * m[np.abs(f - t) < 30].sum() / m[f > 80].sum():4.0f}%" for t in tones)
    print(f"{k / rate:5.2f}s rms {rms:6.1f}  peak {f[np.argmax(m * (f > 80))]:5.0f} Hz  {shares}")
