#!/bin/bash
SP=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/9231c63a-2a37-48b6-9847-1c1f92c8caf3/scratchpad
O=$SP/par3m/$1/$2; mkdir -p $O/cert; t0=$(python3 -c 'import time;print(time.time())')
case "$4" in
  direct-tuner) $SP/par3m/ejmap --cert-tuner "$3" --out $O/cert --probe $SP/probe-rd > $O/run.txt 2>&1 ;;
  multiband) echo '[{"product":"C6 (s)","state":"multiband","reason":"seeded for the parallel test (10 Oct): the compressor loop files this on Sean'"'"'s Mac"}]' > $O/cert/outcomes.json
             $SP/par3m/ejmap --phaseb-all --category multiband --only "$3" --out $O/cert --probe $SP/probe-rd --ejmap-ledger ~/Library/ejmap > $O/run.txt 2>&1 ;;
  timing) cp -R $SP/par3m/seed_sbc/fixtures $SP/par3m/seed_sbc/outcomes.json $O/cert/
          $SP/par3m/ejmap --phaseb-all --category timing --only "$3" --out $O/cert --probe $SP/probe-rd --ejmap-ledger ~/Library/ejmap > $O/run.txt 2>&1 ;;
esac
rc=$?; t1=$(python3 -c 'import time;print(time.time())'); echo "$2 rc=$rc start=$t0 end=$t1" >> $SP/par3m/$1/times.txt
