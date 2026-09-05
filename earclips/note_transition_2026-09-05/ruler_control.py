"""POSITIVE CONTROL for the wrong-note instrument (round 56): plant a +c cent,
60 ms excursion at N fully voiced sites of the SOURCE (local resample of the
window at 2^(c/1200), 2 ms edge cross-fades), track the planted file with the
same ruler, and count how many plants the instrument reports as events.
Usage: python ruler_control.py <dir> <npz from ruler_run>"""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ruler_run import read_mono, track, events
d = sys.argv[1]; v = np.load(sys.argv[2])
sr = 48000; hop = 128
x, _ = read_mono(os.path.join(d, 'source_different_bit.wav'))
f0src = v['f0_src']; vs = v['vs']; starts = v['starts']
# fully voiced sites: frame i whose +-100 frames (~270 ms) are all voiced (in all three files)
ok = np.array([(i >= 100 and i + 100 <= len(vs) and vs[i-100:i+100].all()) for i in range(len(vs))])
cand = np.where(ok)[0]
sites = cand[np.linspace(0, len(cand) - 1, 21).astype(int)] if len(cand) >= 21 else cand
print('fully voiced candidate frames: %d, plant sites: %d' % (len(cand), len(sites)))
C0 = 1200 * np.log2(np.maximum(f0src, 1e-9) / 440.0); tgt = 100 * np.round(C0 / 100)
def plant(x, s0, cents, ms=60.0):
    y = x.copy(); n = int(ms * sr / 1000); r = 2.0 ** (cents / 1200.0)
    idx = s0 + np.arange(n) * r; src = np.interp(idx, np.arange(len(x)), x)
    fade = int(0.002 * sr); w = np.ones(n); w[:fade] = np.linspace(0, 1, fade); w[-fade:] = np.linspace(1, 0, fade)
    y[s0:s0 + n] = x[s0:s0 + n] * (1 - w) + src * w
    return y
for cents in (100.0, 60.0, 45.0):
    fired = 0
    for fi in sites:
        s0 = int(starts[fi]) + 1024 - int(0.030 * sr)   # centre the 60 ms plant on the frame centre
        y = plant(x, s0, cents)
        f0p, vp, st = track(y, sr, hop)
        Cp = 1200 * np.log2(np.maximum(f0p, 1e-9) / 440.0)
        err = {'src': np.where(vs, C0 - tgt, np.nan), 'p': np.where(vs, Cp - tgt, np.nan)}
        ev = events(err, vs, 'p', hop=hop, sr=sr)
        hit = any(a <= fi + 12 and b >= fi - 12 for a, b in ev)   # an event overlapping the plant (+-32 ms)
        fired += 1 if hit else 0
    print('plant %+.0fc / 60 ms at %d fully voiced sites: the instrument fires %d/%d' % (cents, len(sites), fired, len(sites)))
