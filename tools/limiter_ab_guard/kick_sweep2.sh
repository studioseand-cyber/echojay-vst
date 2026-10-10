#!/bin/bash
# kick_sweep2.sh "<label>|<render options>" ... : like kick_sweep.sh, but every variant is ALSO rendered and judged on
# bass_sustain (gain 8.5): level, median retention, momentary GR mean/std, t63/t90. Pro-L 2 first.
cd "$(dirname "$0")/../.."
R=docs/limiter_ab/renders; B=build-limiter-ab; S=$(mktemp -d)
bass() { python3 - "$1" "$2" <<'PY'
import sys,re,statistics
txt=open(sys.argv[1]).read().splitlines(); tag=sys.argv[2]; on=False; ret=[]; t63=[]; t90=[]; dyn=lv=tp=None
for l in txt:
    if re.match(r'^== case bass_sustain\s+tag '+re.escape(tag)+r'\s',l): on=True; continue
    if on and l.startswith('ROW'): break
    if not on: continue
    m=re.match(r'^\s+(\d+)\s+([\d.]+)\s+(-?[\d.]+|n/a)\s+(-?[\d.]+|n/a)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)',l)
    if m and 'GR trace' not in l and m.group(3)!='n/a' and float(m.group(3))>0:
        ret.append(float(m.group(4))); 
        if m.group(11)!='n/a': t63.append(float(m.group(11)))
        if m.group(12)!='n/a': t90.append(float(m.group(12)))
    if 'dynamics' in l: dyn=l
    if 'loudness' in l: lv=l
    if 'peaks' in l: tp=l
gm=re.search(r'GR mean (\S+) dB  max \S+  std (\S+)',dyn or ''); lvm=re.search(r'out  I (\S+) LUFS',lv or ''); tpm=re.search(r'exact\) (\S+) dBTP',tp or '')
print(f"BASS level {lvm.group(1) if lvm else '?'} | ret med {statistics.median(ret) if ret else float('nan'):6.2f} | GR mean/std {gm.group(1) if gm else '?'}/{gm.group(2) if gm else '?'} | t63/t90 {statistics.median(t63) if t63 else float('nan'):4.1f}/{statistics.median(t90) if t90 else float('nan'):5.1f} | TP {tpm.group(1) if tpm else '?'}")
PY
}
$B/limiter_ab_guard analyse $R --case fullmix_hot --gain 10.86 --ceiling 0.0 --trace > $S/h.txt 2>&1
$B/limiter_ab_guard analyse $R --case bass_sustain --gain 8.5 --ceiling 0.0 --trace > $S/b.txt 2>&1
printf "%-30s HOT  " "Pro-L 2"; python3 tools/limiter_ab_guard/kick_summ.py $S/h.txt proL2; printf "%-30s " ""; bass $S/b.txt proL2
for spec in "$@"; do
  label="${spec%%|*}"; opts="${spec#*|}"
  $B/limiter_v2_render $R/source_fullmix_hot.wav $R/v2_fullmix_hot.wav --gain 10.86 --ceiling 0.0 --tp 1 $opts > /dev/null
  $B/limiter_v2_render $R/source_bass_sustain.wav $R/v2_bass_sustain.wav --gain 8.5 --ceiling 0.0 --tp 1 $opts > /dev/null
  $B/limiter_ab_guard analyse $R --case fullmix_hot --gain 10.86 --ceiling 0.0 --trace > $S/h.txt 2>&1
  $B/limiter_ab_guard analyse $R --case bass_sustain --gain 8.5 --ceiling 0.0 --trace > $S/b.txt 2>&1
  printf "%-30s HOT  " "$label"; python3 tools/limiter_ab_guard/kick_summ.py $S/h.txt v2; printf "%-30s " ""; bass $S/b.txt v2
done
rm -rf $S
