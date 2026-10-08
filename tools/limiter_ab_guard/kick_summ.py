#!/usr/bin/env python3
# kick_summ.py <analyse output> <tag>: per-hit metrics from the harness's --trace output for one case/tag:
# mean GR re baseline at +1/+3/+8/+20 ms (1 ms trace), median t63, momentary GR mean/std, integrated level, median retention, TP.
import sys, re, statistics
txt = open(sys.argv[1]).read().splitlines(); tag = sys.argv[2]
on = False; hits = []; traces = []; cur = None; dyn = lv = tp = None
for l in txt:
    if re.match(r'^== case \S+\s+tag ' + re.escape(tag) + r'\s', l): on = True; continue
    if on and l.startswith('ROW'): break
    if not on: continue
    m = re.match(r'^\s+(\d+)\s+([\d.]+)\s+(-?[\d.]+|n/a)\s+(-?[\d.]+|n/a)\s+(-?[\d.]+|n/a)\s+(-?[\d.]+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)', l)
    if m and 'GR trace' not in l:
        cur = {'over': float(m.group(3)) if m.group(3) != 'n/a' else None, 'ret': float(m.group(4)) if m.group(4) != 'n/a' else None, 't63': m.group(10)}
        hits.append(cur); traces.append([]); continue
    mt = re.match(r'^\s+([+-]\d+) ms\s+(.*)$', l)
    if mt and traces: vals = [None if v == 'n/a' else float(v) for v in mt.group(2).split()]; traces[-1].extend(vals); continue
    if 'dynamics' in l: dyn = l
    if 'loudness' in l: lv = l
    if 'peaks' in l: tp = l
def at(ms):
    v = [t[ms + 25] for t, h in zip(traces, hits) if len(t) > ms + 25 and t[ms + 25] is not None and h['over'] is not None and h['over'] > 0]
    return statistics.mean(v) if v else float('nan')
t63 = [float(h['t63']) for h in hits if h['t63'] not in ('n/a',) and h['over'] is not None and h['over'] > 0]
ret = [h['ret'] for h in hits if h['ret'] is not None and h['over'] is not None and h['over'] > 0]
gm = re.search(r'GR mean (\S+) dB  max \S+  std (\S+)', dyn or ''); lvm = re.search(r'out  I (\S+) LUFS', lv or ''); tpm = re.search(r'TRUE PEAK \(arbiter, exact\) (\S+) dBTP', tp or '')
print(f"GR +1/+3/+8/+20 ms {at(1):6.2f}/{at(3):6.2f}/{at(8):6.2f}/{at(20):6.2f} | t63 med {statistics.median(t63) if t63 else float('nan'):4.1f} | GR mean/std {gm.group(1) if gm else '?'}/{gm.group(2) if gm else '?'} | level {lvm.group(1) if lvm else '?'} | ret med {statistics.median(ret) if ret else float('nan'):6.2f} | TP {tpm.group(1) if tpm else '?'}  (hits over: {len(ret)})")
