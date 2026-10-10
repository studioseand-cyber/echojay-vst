#!/bin/bash
# one.sh <pass> <key> <product> <category|direct>
SP=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/9231c63a-2a37-48b6-9847-1c1f92c8caf3/scratchpad
O=$SP/par/$1/$2; mkdir -p $O; t0=$(python3 -c 'import time;print(time.time())')
if [ "$4" = direct ]; then $SP/par/ejmap --cert-limiter-comp "$3" --out $O/cert --probe $SP/probe-rd --ejmap-ledger ~/Library/ejmap > $O/run.txt 2>&1
else $SP/par/ejmap --phaseb-all --category "$4" --only "$3" --out $O/cert --probe $SP/probe-rd --ejmap-ledger ~/Library/ejmap > $O/run.txt 2>&1; fi
rc=$?; t1=$(python3 -c 'import time;print(time.time())'); echo "$2 rc=$rc start=$t0 end=$t1" >> $SP/par/$1/times.txt
