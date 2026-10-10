#!/bin/bash
# one.sh <pass> <key> <product> <phaseb category>; timing is seeded with the certified bx_opto (fixtures + outcomes) first
SP=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/9231c63a-2a37-48b6-9847-1c1f92c8caf3/scratchpad
O=$SP/par2m/$1/$2; mkdir -p $O/cert; t0=$(python3 -c 'import time;print(time.time())')
if [ "$4" = timing ]; then cp -R $SP/par/serialA/comp/cert/fixtures $SP/par/serialA/comp/cert/outcomes.json $O/cert/; fi
$SP/par2m/ejmap --phaseb-all --category "$4" --only "$3" --out $O/cert --probe $SP/probe-rd --ejmap-ledger ~/Library/ejmap > $O/run.txt 2>&1
rc=$?; t1=$(python3 -c 'import time;print(time.time())'); echo "$2 rc=$rc start=$t0 end=$t1" >> $SP/par2m/$1/times.txt
