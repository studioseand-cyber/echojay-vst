#!/bin/bash
# make_blind_styles.sh <style> [seed]: blind listening files for one style in ~/echojay-limiter-renders/blind_styles/<style>/:
#   per case (fullmix_hot at +10.87, bass_sustain at +8.45): a LEVEL-MATCHED pair (Pro-L 2's print vs ours, both scaled down to
#   the quieter's integrated LUFS) as <case>_matched_{A,B}.wav, and an UNMATCHED pair at the same input gain as
#   <case>_unmatched_{A,B}.wav. A/B by the seed; KEY.txt (with every file's integrated LUFS) in the same folder, nothing else
#   says which is which. Offline only: existing binaries.
set -e
cd "$(dirname "$0")/../.."
ST="$1"; SEED="${2:-20261009}"; R=docs/limiter_ab/renders/styles/$ST; B=build-limiter-ab; OUT=$HOME/echojay-limiter-renders/blind_styles/$ST; S=$(mktemp -d)
mkdir -p "$OUT"; rm -f "$OUT"/*.wav "$OUT"/KEY.txt
echo "# blind files for the $ST style, $(date '+%Y-%m-%d %H:%M'); seed $SEED. matched = both scaled DOWN to the quieter's integrated LUFS; unmatched = as rendered at the same input gain" > "$OUT/KEY.txt"
n=0
for CASE in fullmix_hot bass_sustain; do
  G=10.87; [ $CASE = bass_sustain ] && G=8.45
  $B/limiter_v2_render $R/source_$CASE.wav $S/ours_$CASE.wav --style $ST --gain $G --ceiling 0.0 --tp 1 > /dev/null
  # matched pair: blind_pack scales both to the quieter and shuffles A/B
  T=$(mktemp -d); $B/blind_pack "$T" $CASE $((SEED+n)) proL2=$R/proL2_$CASE.wav ours=$S/ours_$CASE.wav > "$T/log.txt"
  mv "$T/${CASE}_A.wav" "$OUT/${CASE}_matched_A.wav"; mv "$T/${CASE}_B.wav" "$OUT/${CASE}_matched_B.wav"
  echo "case $CASE MATCHED (seed $((SEED+n))):" >> "$OUT/KEY.txt"; sed -n '2,$p' "$T/KEY.txt" | sed "s/${CASE}_\([AB]\)\.wav/${CASE}_matched_\1.wav/" >> "$OUT/KEY.txt"; rm -rf "$T"
  # unmatched pair: one blind_pack call with --unmatched (no scaling, both padded to one length), A/B by the seed
  T=$(mktemp -d); $B/blind_pack "$T" $CASE $((SEED+n+100)) proL2=$R/proL2_$CASE.wav ours=$S/ours_$CASE.wav --unmatched > "$T/log.txt"
  mv "$T/${CASE}_A.wav" "$OUT/${CASE}_unmatched_A.wav"; mv "$T/${CASE}_B.wav" "$OUT/${CASE}_unmatched_B.wav"
  echo "case $CASE UNMATCHED (input gain +$G dB, seed $((SEED+n+100))):" >> "$OUT/KEY.txt"; sed -n '2,$p' "$T/KEY.txt" | sed "s/${CASE}_\([AB]\)\.wav/${CASE}_unmatched_\1.wav/" >> "$OUT/KEY.txt"; rm -rf "$T"
  n=$((n+2))
done
rm -rf "$S"; ls -l "$OUT" | awk 'NR>1{print $5, $9}'; echo "(key in $OUT/KEY.txt, not printed)"
