#!/bin/bash
# make_blind.sh [seed]: level-matched blind files for fullmix_hot and bass_sustain in ~/echojay-limiter-renders/blind/
#   A/B/C/D per case = Pro-L 2 print, v2 Transparent, v2 Clean, the current limiter (the Pro Tools print for bass,
#   the proven legacy port for the hot mix), shuffled with the seed; KEY.txt in the same folder.
set -e
cd "$(dirname "$0")/../.."
SEED="${1:-20261008}"; R=docs/limiter_ab/renders; B=build-limiter-ab; OUT=$HOME/echojay-limiter-renders/blind; S=$(mktemp -d)
mkdir -p "$OUT"; rm -f "$OUT"/*.wav "$OUT"/KEY.txt
echo "# blind listening files, $(date '+%Y-%m-%d %H:%M'); seed $SEED; every file scaled DOWN to the quietest of its case" > "$OUT/KEY.txt"
$B/limiter_v2_render $R/source_fullmix_hot.wav $S/v2t_hot.wav   --style transparent --gain 10.86 --ceiling 0.0 --tp 1 >/dev/null
$B/limiter_v2_render $R/source_fullmix_hot.wav $S/v2c_hot.wav   --style clean       --gain 10.86 --ceiling 0.0 --tp 1 >/dev/null
$B/limiter_v2_render $R/source_bass_sustain.wav $S/v2t_bass.wav --style transparent --gain 8.5   --ceiling 0.0 --tp 1 >/dev/null
$B/limiter_v2_render $R/source_bass_sustain.wav $S/v2c_bass.wav --style clean       --gain 8.5   --ceiling 0.0 --tp 1 >/dev/null
$B/blind_pack "$OUT" fullmix_hot  $SEED proL2=$R/proL2_fullmix_hot.wav v2transparent=$S/v2t_hot.wav  v2clean=$S/v2c_hot.wav  current=$R/echojay_fullmix_hot.wav
$B/blind_pack "$OUT" bass_sustain $((SEED+1)) proL2=$R/proL2_bass_sustain.wav v2transparent=$S/v2t_bass.wav v2clean=$S/v2c_bass.wav current=$R/echojay_bass_sustain.wav
rm -rf "$S"; ls -l "$OUT"; echo; cat "$OUT/KEY.txt"
