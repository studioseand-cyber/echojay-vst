"""Driver for the reviewer's rulers (round 56): tracks the three files with
ruler_vtrack (continuity-constrained YIN), builds the frame table, and runs
the wrong-note instrument + event classification. Usage:
    python ruler_run.py <dir> [out.npz]      (files: source/antares/echojay_different_bit.wav)
"""
import sys, os, wave, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ruler_vtrack import cmnd_matrix, viterbi

def _read_float_wav(path):
    # the stdlib wave module refuses IEEE-float (format 3): a minimal RIFF walk
    import struct
    b = open(path, 'rb').read(); pos = 12; fmt = None; data = None
    while pos + 8 <= len(b):
        tag = b[pos:pos+4]; sz = struct.unpack('<I', b[pos+4:pos+8])[0]; body = b[pos+8:pos+8+sz]
        if tag == b'fmt ': fmt = struct.unpack('<HHIIHH', body[:16])
        elif tag == b'data': data = body
        pos += 8 + sz + (sz & 1)
    if fmt is None or data is None or fmt[0] != 3 or fmt[5] != 32: raise SystemExit('unsupported wav ' + path)
    ch = fmt[1]; sr = fmt[2]
    v = np.frombuffer(data, dtype='<f4').astype(np.float64)
    v = v.reshape(-1, ch).mean(axis=1) if ch > 1 else v
    return v, sr

def read_mono(path):
    try:
        w = wave.open(path)
    except wave.Error:
        return _read_float_wav(path)
    n = w.getnframes(); ch = w.getnchannels(); sw = w.getsampwidth(); sr = w.getframerate()
    raw = w.readframes(n); w.close()
    if sw == 3:
        a = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (a[:, 0].astype(np.int32) | (a[:, 1].astype(np.int32) << 8) | (a[:, 2].astype(np.int32) << 16))
        v = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / (1 << 23)
    elif sw == 2: v = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    elif sw == 4: v = np.frombuffer(raw, dtype=np.float32).astype(np.float64)
    else: raise SystemExit('sample width %d' % sw)
    v = v.reshape(-1, ch).mean(axis=1) if ch > 1 else v   # the reviewer's front end: the MEAN of the two channels (round 56, matched exactly)
    return v, sr

def track(x, sr, hop=128):
    M, starts, rms, tmin, tmax = cmnd_matrix(x, sr, W=2048, hop=hop)
    f0, voiced = viterbi(M, starts, rms, tmin, tmax, sr)
    return f0, voiced, starts

def events(err, vs, k, thr=40.0, minms=25.0, hop=128, sr=48000, suboctave=True):
    bad = vs & (np.abs(err[k]) > thr) & (np.abs(err[k]) > np.abs(err['src']))
    if suboctave: bad &= (np.abs(err[k]) < 250)
    idx = np.where(bad)[0]; out = []
    if not len(idx): return out
    s = p = idx[0]
    for i in idx[1:]:
        if i - p > 3:
            if (p - s) * hop / sr * 1000 >= minms: out.append((s, p))
            s = i
        p = i
    if (p - s) * hop / sr * 1000 >= minms: out.append((s, p))
    return out

def analyse(f0, vs, t, hop=128, sr=48000, verbose=True):
    C = {k: 1200 * np.log2(np.maximum(f0[k], 1e-9) / 440.0) for k in f0}
    tgt = 100 * np.round(C['src'] / 100)
    err = {}
    for k in f0:
        e = np.full(len(t), np.nan); e[vs] = C[k][vs] - tgt[vs]; err[k] = e
    med = {k: float(np.median(np.abs(err[k][vs]))) for k in f0}
    tt = tgt.copy(); tt[~vs] = np.nan
    ch = np.where(np.abs(np.diff(np.nan_to_num(tt, nan=0))) >= 50)[0]
    trans = np.zeros(len(t), bool)
    for i in ch: trans[max(0, i - 1):i + 2] = True
    res = {'median': med, 'events': {}}
    for k in [x for x in f0 if x != 'src']:
        ev = events(err, vs, k, hop=hop, sr=sr)
        w = int(0.120 * sr / hop)
        near = sum(1 for a, b in ev if trans[max(0, a - w):min(len(t), b + w)].any())
        tot = sum((b - a) * hop / sr * 1000 for a, b in ev)
        res['events'][k] = (len(ev), tot, near, ev)
        if verbose:
            print('  %s: %d sub-octave wrong-note events >=25 ms, total %.0f ms; %d of %d within 120 ms of a source note change' % (k.upper(), len(ev), tot, near, len(ev)))
            for a, b in sorted(ev, key=lambda r: -(r[1] - r[0])):
                pk = np.nanargmax(np.abs(err[k][a:b + 1])) + a
                n = 'TRANSITION' if trans[max(0, a - w):min(len(t), b + w)].any() else 'sustain'
                print('     t %.3f-%.3f (%3.0f ms) peak %+7.1fc  src note %+.0f -> out note %+.0f  [%s]' % (t[a], t[b], (b - a) * hop / sr * 1000, err[k][pk], tgt[pk] / 100, np.round(C[k][pk] / 100), n))
    if verbose:
        print('  median |error vs the source\'s own note|: ' + '  '.join('%s %.2fc' % (k, med[k]) for k in ('src', 'ant', 'ecj') if k in med))
    return res, err, tgt, C

if __name__ == '__main__':
    d = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else None
    sr = 48000; hop = 128
    f0 = {}; voiced = {}; starts = None
    for k, name in (('src', 'source'), ('ant', 'antares'), ('ecj', 'echojay')):
        x, sr = read_mono(os.path.join(d, name + '_different_bit.wav'))
        f0[k], voiced[k], starts = track(x, sr, hop)
        print('tracked %s: %d frames, %d voiced' % (k, len(starts), int(voiced[k].sum())))
    # The reviewer's table: t = frame centre (+W/2), vs = voiced in ALL THREE files. Matched exactly in round 56.
    t = (starts + 1024) / sr; vs = voiced['src'] & voiced['ant'] & voiced['ecj']
    print('\n=== t = frame centre, vs = all three voiced (%d frames) ===' % int(vs.sum()))
    analyse(f0, vs, t, hop, sr)
    if out:
        np.savez(out, t=(starts + 1024) / sr, vs=voiced['src'] & voiced['ant'] & voiced['ecj'], vs_src=voiced['src'], f0_src=f0['src'], f0_ant=f0['ant'], f0_ecj=f0['ecj'], starts=starts)
        print('saved', out)
