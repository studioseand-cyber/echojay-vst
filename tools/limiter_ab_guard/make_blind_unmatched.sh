#!/bin/bash
# make_blind_unmatched.sh <outdir> <case> <gain> <seed> [proL2=<file>] - UNMATCHED blind pack: the four limiters at the
# SAME input gain as the Pro-L 2 print, no loudness matching, written as float32 under neutral names A-D, with KEY.txt
# (and the integrated LUFS of each file) in the same folder. Offline only: existing binaries, no build.
#   v2 Transparent and v2 Clean are rendered now (the post-fix engine); the current limiter is the Pro Tools print where
#   one exists (bass) and the proven legacy port otherwise (rendered now at the gain).
set -e
cd "$(dirname "$0")/../.."
OUT="$1"; CASE="$2"; GAIN="$3"; SEED="$4"; PROL2="${5#proL2=}"
R=docs/limiter_ab/renders; B=build-limiter-ab; S=$(mktemp -d)
mkdir -p "$OUT"
SRC=$R/source_$CASE.wav; [ -f "$SRC" ] || { echo "no $SRC"; exit 2; }
[ -n "$PROL2" ] || PROL2=$R/proL2_$CASE.wav
$B/limiter_v2_render $SRC $S/v2t.wav --style transparent --gain $GAIN --ceiling 0.0 --tp 1 >/dev/null
$B/limiter_v2_render $SRC $S/v2c.wav --style clean       --gain $GAIN --ceiling 0.0 --tp 1 >/dev/null
if [ -f $R/echojay_$CASE.wav ] && [ "$CASE" != "fullmix_hot" ] && [ "$CASE" != "fullmix_push" ]; then CUR=$R/echojay_$CASE.wav; CURNOTE="Pro Tools print"; else $B/limiter_legacy_render $SRC $S/cur.wav --gain $GAIN --ceiling 0.0 --tp 1 >/dev/null; CUR=$S/cur.wav; CURNOTE="legacy port, rendered"; fi
# float32 copies + LUFS, one blind_pack call per file (a lone file scales by 0 dB), then the shuffle
declare -a LABELS=(proL2 v2transparent v2clean current) FILES=("$PROL2" "$S/v2t.wav" "$S/v2c.wav" "$CUR")
ORDER=$(python3 -c "
import random; r=random.Random($SEED); o=list(range(4)); r.shuffle(o); print(' '.join(map(str,o)))")
KEY="$OUT/KEY.txt"; echo "case $CASE  (UNMATCHED: every file at input gain +$GAIN dB, no loudness matching; seed $SEED; $(date '+%Y-%m-%d %H:%M'))" >> "$KEY"
k=0
for idx in $ORDER; do
  L=$(printf "\\x$(printf %x $((65+k)))"); T=$(mktemp -d); $B/blind_pack "$T" tmp 1 "${LABELS[$idx]}=${FILES[$idx]}" > "$T/log.txt"
  LUFS=$(grep -oE "\(-?[0-9.]+ LUFS" "$T/log.txt" | head -1 | tr -d "(" | awk '{print $1}')
  mv "$T/tmp_A.wav" "$OUT/${CASE}_$L.wav"; rm -rf "$T"
  echo "  ${CASE}_$L.wav = ${LABELS[$idx]} ${FILES[$idx]} ($([ "${LABELS[$idx]}" = current ] && echo "$CURNOTE; ")integrated $LUFS LUFS)" >> "$KEY"
  echo "${CASE}_$L.wav  integrated $LUFS LUFS"
  k=$((k+1))
done
echo >> "$KEY"; rm -rf "$S"
