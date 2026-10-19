"""Finds isolated bursts in a recording; prints their spacing, duration and dominant frequency."""
import sys, wave
import numpy as np
with wave.open(sys.argv[1]) as w:
    rate = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float) / 32768
x = x[int(3 * rate):]
x -= x.mean()
n = rate // 2000  # 0.5 ms
env = np.array([np.sqrt(np.mean(x[k:k + n] ** 2)) for k in range(0, len(x) - n, n)])
thr = max(np.median(env) * 4, env.max() * 0.15)
on = env > thr
starts = [i for i in range(1, len(on)) if on[i] and not on[i - 1]]
# merge bursts closer than 3 ms, measure duration until the envelope stays below threshold for 3 ms
bursts = []
for s in starts:
    if bursts and s - bursts[-1][1] < 6:
        continue
    e = s
    while e < len(on) and on[e:e + 6].any():
        e += 1
    bursts.append((s, e))
d = np.array([(e - s) * 0.5 for s, e in bursts])
sp = np.diff([s * 0.5 for s, _ in bursts])
print(f"{len(bursts)} bursts, spacing median {np.median(sp):.1f} ms, duration median {np.median(d):.1f} ms "
      f"(p25 {np.percentile(d, 25):.1f}, p75 {np.percentile(d, 75):.1f})")
freqs = []
for s, e in bursts:
    seg = x[s * n:e * n]
    if len(seg) < 32:
        continue
    seg = np.pad(seg * np.hanning(len(seg)), (0, 8192 - len(seg))) if len(seg) < 8192 else seg
    m = np.abs(np.fft.rfft(seg))
    f = np.fft.rfftfreq(len(seg), 1 / rate)
    freqs.append(f[np.argmax(m * (f > 150))])
print("dominant Hz per burst: median %.0f, values %s" % (np.median(freqs), " ".join(f"{v:.0f}" for v in freqs[:12])))
