"""Round 58: counts the corrector's TARGET-DEGREE switches in a PA_TRACE dump
(tools/pitch_activity, PA_TRACE=t0,t1), plus how many hops had a pending and
how many selected provisionally. Usage: pitch_activity ... | grep '^ *[0-9]\\.[0-9][0-9][0-9] |' | python count_target_switches.py <seconds>"""
import sys, re
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 2.6
deg = []; pend = 0; prov = 0; n = 0
for line in sys.stdin:
    cols = line.split('|')
    if len(cols) < 6: continue
    m = re.match(r'\s*(\S+)\s+(\S+)\s+(\S+)\(\s*(\S+)\)\s+(\S+)\s+(\S+)\s+(\d)', cols[3])
    if not m: continue
    deg.append(float(m.group(6))); prov += int(m.group(7)); n += 1
    pend += 1 if cols[4].strip().startswith('1') else 0
sw = sum(1 for a, b in zip(deg, deg[1:]) if a != b)
print('target-degree switches %d in %.1f s (%.1f/s); hops with a pending %d/%d; provisional selection %d/%d' % (sw, secs, sw / secs, pend, n, prov, n))
