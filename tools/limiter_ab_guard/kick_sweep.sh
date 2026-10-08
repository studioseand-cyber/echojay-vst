#!/bin/bash
# kick_sweep.sh "<label>|<render options>" ...  : fullmix_hot through v2 with each option set, then the harness's hit
# table; prints per variant the mean over hits of GR at +1/+3/+8/+20 ms (from the 1 ms traces), median t63, the
# momentary GR mean/std, integrated level, median retention, arbiter TP. Pro-L 2's row first.
cd "$(dirname "$0")/../.."
R=docs/limiter_ab/renders; B=build-limiter-ab; S=$(mktemp -d)
summ() { python3 tools/limiter_ab_guard/kick_summ.py "$1" "$2"; }
$B/limiter_ab_guard analyse $R --case fullmix_hot --gain 10.86 --ceiling 0.0 --trace > $S/base.txt 2>&1
printf "%-34s " "Pro-L 2"; summ $S/base.txt proL2
for spec in "$@"; do
  label="${spec%%|*}"; opts="${spec#*|}"
  $B/limiter_v2_render $R/source_fullmix_hot.wav $R/v2_fullmix_hot.wav --gain 10.86 --ceiling 0.0 --tp 1 $opts > /dev/null
  $B/limiter_ab_guard analyse $R --case fullmix_hot --gain 10.86 --ceiling 0.0 --trace > $S/v.txt 2>&1
  printf "%-34s " "$label"; summ $S/v.txt v2
done
rm -rf $S
