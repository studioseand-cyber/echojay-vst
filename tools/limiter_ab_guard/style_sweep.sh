#!/bin/bash
# style_sweep.sh <style> "<label>|<render options>" ... : render every variant on the style's five cases (hot 10.87, bass 8.45,
# probe/tones 8.2) into renders/styles/<style>/v2_<case>.wav, analyse against the Pro-L 2 print, print one block per variant:
# hot kick summary, bass kick summary, then probe / tone_50 / tone_997 rows (level, t63/t90, THD). The Pro-L 2 target first.
cd "$(dirname "$0")/../.."
ST="$1"; shift; R=docs/limiter_ab/renders/styles/$ST; B=build-limiter-ab; S=$(mktemp -d)
gain_for() { case "$1" in bass_sustain) echo 8.45;; fullmix_hot) echo 10.87;; *) echo 8.2;; esac; }
row() { python3 -c "
import re,sys
for l in open(sys.argv[1]):
    if l.startswith('ROW case='+sys.argv[2]+' tag='+sys.argv[3]+' '):
        g=lambda k: (re.search(k+r'=\s*(\S+)',l) or [None,'?'])[1]
        print('level',g('outI'),'GRmean',g('grMean'),'std',g('grStd'),'ret',g('retMed'),'t63/t90',g('t63med')+'/'+g('t90med'),'tp',g('tp'),'overs',g('overs'))" "$1" "$2" "$3"; }
thd() { python3 -c "
import re,sys
for l in open(sys.argv[1]):
    if l.startswith('ROW case='+sys.argv[2]+' tag='+sys.argv[3]+' '):
        g=lambda k: (re.search(k+r'=\s*(\S+)',l) or [None,'?'])[1]
        print('level',g('outI'),'THD',g('thdMax'),'tp',g('tp'))" "$1" "$2" "$3"; }
summarise() { local tag=$1 label=$2
  printf "%-28s HOT   " "$label"; python3 tools/limiter_ab_guard/kick_summ.py $S/fullmix_hot.txt $tag
  printf "%-28s BASS  " "";       python3 tools/limiter_ab_guard/kick_summ.py $S/bass_sustain.txt $tag
  printf "%-28s bass  %s\n" "" "$(row $S/bass_sustain.txt bass_sustain $tag)"
  printf "%-28s probe %s | tone_50 %s | tone_997 %s\n" "" "$(row $S/probe_transients.txt probe_transients $tag)" "$(thd $S/tone_50.txt tone_50 $tag)" "$(thd $S/tone_997.txt tone_997 $tag)"
}
for c in fullmix_hot bass_sustain probe_transients tone_50 tone_997; do $B/limiter_ab_guard analyse $R --case $c --gain $(gain_for $c) --ceiling 0.0 --trace > $S/$c.txt 2>&1; done
summarise proL2 "Pro-L 2 $ST"
for spec in "$@"; do
  label="${spec%%|*}"; opts="${spec#*|}"
  for c in fullmix_hot bass_sustain probe_transients tone_50 tone_997; do
    $B/limiter_v2_render $R/source_$c.wav $R/v2_$c.wav --gain $(gain_for $c) --ceiling 0.0 --tp 1 $opts > /dev/null || { echo "render failed: $c $opts"; continue; }
    $B/limiter_ab_guard analyse $R --case $c --gain $(gain_for $c) --ceiling 0.0 --trace > $S/$c.txt 2>&1
  done
  summarise v2 "$label"; echo "  overs: $(for c in fullmix_hot bass_sustain probe_transients tone_50 tone_997; do grep "^ROW case=$c tag=v2" $S/$c.txt | sed -E 's/.*overs=([0-9]+).*/\1/' | tr '\n' ' '; done)"
done
rm -rf $S
