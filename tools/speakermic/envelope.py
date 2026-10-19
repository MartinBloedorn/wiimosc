import sys, wave
import numpy as np
with wave.open(sys.argv[1]) as w:
    rate = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float) / 32768
x = x[int(3 * rate):]
x -= x.mean()
# 1 ms envelope
n = rate // 1000
env = np.array([np.sqrt(np.mean(x[k:k + n] ** 2)) for k in range(0, len(x) - n, n)])
spec = np.abs(np.fft.rfft(env - env.mean()))
f = np.fft.rfftfreq(len(env), 1e-3)
top = np.argsort(spec * (f > 5))[::-1][:6]
print("envelope modulation peaks Hz:", ", ".join(f"{f[i]:.1f}" for i in top))
# envelope sketch over 100 ms (1 char per 2 ms)
seg = env[1000:1100]
levels = " .:-=+*#%@"
m = seg.max()
print("100 ms envelope: |" + "".join(levels[min(9, int(9 * v / m))] for v in seg[::2]) + "|")
