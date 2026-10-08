#!/bin/bash
# make_blind_push.sh [seed]: when docs/limiter_ab/renders/proL2_fullmix_push.wav exists (Pro-L 2 at ~+15 dB effective),
# estimate its gain from unlimited blocks, then write BOTH a level-matched pack and an unmatched pack to
# ~/echojay-limiter-renders/blind_push/ (matched: fullmix_push_matched_A..D; unmatched: fullmix_push_A..D) with one
# KEY.txt. Offline only, existing binaries. The case name "fullmix_push" uses source_fullmix via a symlink.
set -e
cd "$(dirname "$0")/../.."
SEED="${1:-20261010}"; R=docs/limiter_ab/renders; B=build-limiter-ab; OUT=$HOME/echojay-limiter-renders/blind_push
[ -f $R/proL2_fullmix_push.wav ] || { echo "proL2_fullmix_push.wav is not in $R yet"; exit 2; }
[ -e $R/source_fullmix_push.wav ] || ln -s source_fullmix.wav $R/source_fullmix_push.wav
GAIN=$(/tmp/gainest $R/source_fullmix.wav $R/proL2_fullmix_push.wav | grep -oE "median \+[0-9.]+" | awk '{print $2}' | tr -d +)
echo "estimated effective gain of the push print: +$GAIN dB (median of unlimited 100 ms blocks)"
rm -rf "$OUT"; mkdir -p "$OUT"
echo "# blind_push: Pro-L 2 print at an estimated +$GAIN dB effective; $(date '+%Y-%m-%d %H:%M')" > "$OUT/KEY.txt"
./tools/limiter_ab_guard/make_blind_unmatched.sh "$OUT" fullmix_push "$GAIN" "$SEED"
S=$(mktemp -d)
$B/limiter_v2_render $R/source_fullmix.wav $S/v2t.wav --style transparent --gain $GAIN --ceiling 0.0 --tp 1 >/dev/null
$B/limiter_v2_render $R/source_fullmix.wav $S/v2c.wav --style clean       --gain $GAIN --ceiling 0.0 --tp 1 >/dev/null
$B/limiter_legacy_render $R/source_fullmix.wav $S/cur.wav --gain $GAIN --ceiling 0.0 --tp 1 >/dev/null
$B/blind_pack "$OUT" fullmix_push_matched $((SEED+1)) proL2=$R/proL2_fullmix_push.wav v2transparent=$S/v2t.wav v2clean=$S/v2c.wav current=$S/cur.wav | sed 's/ <- .*//'
rm -rf "$S"; ls -l "$OUT" | awk '{print $5, $9}'; echo "key: $OUT/KEY.txt (not printed)"
