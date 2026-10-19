"""Analyzes a mono 16-bit wav: level, top spectral peaks, and the tone's share of the energy (if --freq given)."""
import argparse
import wave

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("path")
parser.add_argument("--freq", type=float, default=0.0)
parser.add_argument("--skip", type=float, default=0.0, help="seconds to skip at the start")
parser.add_argument("--baseline", help="wav of room noise to compare band levels against")
args = parser.parse_args()


def load(path, skip):
    with wave.open(path) as w:
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    return data[int(skip * rate):], rate


def spectrum(x, rate):
    win = np.hanning(len(x))
    mag = np.abs(np.fft.rfft(x * win)) ** 2
    freqs = np.fft.rfftfreq(len(x), 1.0 / rate)
    return freqs, mag


x, rate = load(args.path, args.skip)
x = x - x.mean()
rms = np.sqrt(np.mean(x ** 2))
print(f"{args.path.split('/')[-1]}: {len(x) / rate:.1f} s, rms {20 * np.log10(rms + 1e-12):.1f} dBFS, "
      f"peak {20 * np.log10(np.abs(x).max() + 1e-12):.1f} dBFS")

freqs, mag = spectrum(x, rate)
band = (freqs > 80)
total = mag[band].sum()

# Energy in coarse bands, against the baseline if given.
edges = [80, 250, 500, 1000, 2000, 4000, 8000]
base = None
if args.baseline:
    bx, brate = load(args.baseline, 0.0)
    bx = bx - bx.mean()
    bfreqs, bmag = spectrum(bx, brate)
    base = (bfreqs, bmag / len(bx))
line = []
for lo, hi in zip(edges[:-1], edges[1:]):
    e = mag[(freqs >= lo) & (freqs < hi)].sum() / len(x)
    s = f"{lo}-{hi}: {10 * np.log10(e + 1e-20):.0f}"
    if base:
        be = base[1][(base[0] >= lo) & (base[0] < hi)].sum()
        s += f" ({10 * np.log10((e + 1e-20) / (be + 1e-20)):+.0f})"
    line.append(s)
print("  bands dB (vs baseline): " + ", ".join(line))

# Top peaks (local maxima, 20 Hz apart).
idx = np.argsort(mag * band)[::-1]
peaks = []
for i in idx:
    if all(abs(freqs[i] - f) > 20 for f in peaks):
        peaks.append(freqs[i])
    if len(peaks) == 6:
        break
print("  top peaks Hz: " + ", ".join(f"{f:.0f}" for f in peaks))

if args.freq:
    near = (np.abs(freqs - args.freq) < 10)
    share = mag[near].sum() / total
    print(f"  {args.freq:.0f} Hz +-10 Hz: {100 * share:.2f}% of energy above 80 Hz")
    # Tone presence over time, 100 ms frames.
    n = rate // 10
    frames = []
    for k in range(0, len(x) - n, n):
        f, m = spectrum(x[k:k + n], rate)
        frames.append(m[np.abs(f - args.freq) < 15].sum() / (m[f > 80].sum() + 1e-20))
    frames = np.array(frames)
    print(f"  tone share per 100 ms: median {100 * np.median(frames):.1f}%, "
          f"frames >20%: {100 * np.mean(frames > 0.2):.0f}%")

# Envelope over time, 50 ms frames, to see choppiness.
n = rate // 20
env = np.array([np.sqrt(np.mean(x[k:k + n] ** 2)) for k in range(0, len(x) - n, n)])
db = 20 * np.log10(env + 1e-12)
print(f"  50 ms envelope dB: min {db.min():.0f}, median {np.median(db):.0f}, max {db.max():.0f}, "
      f"std {db.std():.1f}")
