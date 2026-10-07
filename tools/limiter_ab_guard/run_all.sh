#!/bin/bash
# run_all.sh <label> [--style transparent|clean]: render v2 for every case in docs/limiter_ab/renders at that case's gain,
# analyse each case against Pro-L 2 and the current limiter, save results/<date>_<label>.txt, print the v2 rule lines.
set -e
cd "$(dirname "$0")/../.."
LABEL="${1:-run}"; shift || true
R=docs/limiter_ab/renders; B=build-limiter-ab; OUT=docs/limiter_ab/results/$(date +%Y-%m-%d)_${LABEL}.txt
gain_for() { case "$1" in bass_sustain) echo 8.5;; fullmix_hot) echo 10.86;; *) echo 8.2;; esac; }
CASES="tone_997 tone_50 tone_imd probe_transients panned_transient fullmix bass_sustain fullmix_hot"
{
  echo "# run_all $LABEL $* ($(date '+%Y-%m-%d %H:%M'))  gains: bass_sustain 8.5, fullmix_hot 10.86, others 8.2"
  for c in $CASES; do
    [ -f $R/source_$c.wav ] || { echo "no source for $c"; continue; }
    $B/limiter_v2_render $R/source_$c.wav $R/v2_$c.wav --gain $(gain_for $c) --ceiling 0.0 --tp 1 "$@"
  done
  for c in $CASES; do
    [ -f $R/source_$c.wav ] || continue
    $B/limiter_ab_guard analyse $R --case $c --gain $(gain_for $c) --ceiling 0.0 --trace
  done
} > "$OUT" 2>&1
echo "saved $OUT"
grep -E "^-- rule:|^  (PASS|FAIL)" "$OUT" | awk '/^-- rule/{c=$0; next} {print c " | " $0}' | sed 's/-- rule: //; s/ vs proL2 on / @ /' | grep -v "^proL2tpOFF"
echo "overs by v2 (should all be 0): $(grep -E '^ROW case=.* tag=v2 ' "$OUT" | sed -E 's/.*case=([^ ]+).* overs=([0-9]+).*/\1=\2/' | tr '\n' ' ')"
