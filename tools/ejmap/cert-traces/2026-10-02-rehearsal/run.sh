#!/bin/bash
# THE DRESS REHEARSAL (docs/STRANGER_MAC_TEST.md section C): the stranger's runbook, verbatim, on an empty ledger and an
# empty store, limited to the slice. The only flags beyond the runbook's are the harness's: --ejmap-ledger, --out, --slice.
BIN="/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad/dist/ejmap.app/Contents/MacOS/ejmap"
L=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad/rehearsal/ledger2; C=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad/rehearsal/cert
log() { echo "[$(date +%T)] $*"; }
log "STEP 1 pre-flight"; "$BIN" --cert-preflight
log "STEP 3 census"; "$BIN" --cert-sweep-census $C/fixtures --ejmap-ledger $L 2>&1 | grep -E "worklist:|RUNNABLE:|NOT RUNNABLE|EMO-D5|bx_crispytuner|CL 1B|APB C-18"
log "STEP 4 batch (first invocation, to be interrupted after the second product - criterion A8)"
"$BIN" --cert-sweep-all --profile --ejmap-ledger $L --out $C --slice /private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad/rehearsal/slice.txt > $C/batch-1.log 2>&1 &
BP=$!
while ! [ $(grep -c "^=== \[" $C/batch-1.log 2>/dev/null) -ge 3 ]; do sleep 10; if ! kill -0 $BP 2>/dev/null; then break; fi; done
if kill -0 $BP 2>/dev/null; then log "INTERRUPT: ctrl-C the batch during product 3"; kill -INT $BP; sleep 5; kill -9 $BP 2>/dev/null; fi
wait $BP 2>/dev/null; log "first invocation ended; rows so far: $(python3 -c "import json;print(len(json.load(open('$C/outcomes.json'))))" 2>/dev/null)"
cp $C/outcomes.json $C/outcomes-after-interrupt.json 2>/dev/null
log "STEP 4 batch (second invocation: the same command - resume)"
caffeinate -i "$BIN" --cert-sweep-all --profile --ejmap-ledger $L --out $C --slice /private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad/rehearsal/slice.txt 2>&1 | tee $C/batch-2.log | grep -E "^=== |^  -> |^  wall|SWEEP-ALL|OUTCOMES|INVARIANT|^iLok"
log "STEP 6 zip"; (cd /private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad/rehearsal && zip -rq /private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad/rehearsal/ejmap-cert-rehearsal.zip cert) && unzip -l /private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad/rehearsal/ejmap-cert-rehearsal.zip | grep -c config.json
log "REHEARSAL DONE"
