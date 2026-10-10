#!/bin/bash
# pass.sh <pass> <N>
SP=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/9231c63a-2a37-48b6-9847-1c1f92c8caf3/scratchpad
P=$1; N=$2; mkdir -p $SP/par/$P; : > $SP/par/$P/times.txt
( while [ ! -f $SP/par/$P/DONE ]; do ps -A -o rss=,pcpu=,comm= | awk -v t=$(date +%s) '/ejmap|EchoJayProbe/ {r+=$1; c+=$2; n++} END {print t, n, r, c}' >> $SP/par/$P/samples.txt; vm_stat | awk '/Pages free/ {gsub("\\.","",$3); print "free", $3*16384/1048576}' | tail -1 >> $SP/par/$P/free.txt; sleep 2; done ) &
T0=$(date +%s)
while IFS='|' read -r k p c; do printf '%s\0%s\0%s\0' "$k" "$p" "$c"; done < $SP/par/units.txt | xargs -0 -n 3 -P $N bash $SP/par/one.sh $P
echo "wall $(( $(date +%s) - T0 ))" > $SP/par/$P/wall.txt; touch $SP/par/$P/DONE; wait
