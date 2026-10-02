#!/bin/bash
# THE TEST (docs/STRANGER_MAC_TEST.md): ONE unbroken run from an EMPTY ledger and an EMPTY store, the stranger's runbook
# verbatim - pre-flight, scan + categorise, census, the batch, the zip - with no hand step between stages. The harness's
# only additions: the scratch ledger/cert roots and the slice.
S=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad
BIN="$S/dist/ejmap.app/Contents/MacOS/ejmap"
L=$S/rehearsal/ledger_empty; C=$S/rehearsal/cert_unbroken
log() { echo "[$(date +%T)] $*"; }
rm -rf "$L" "$C"; mkdir -p "$L" "$C"; cp ~/Library/ejmap/config.json "$L"/      # the sign-in token = the one GUI step a fresh Mac does once
log "UNBROKEN START  ledger $L (empty but the sign-in token)  cert $C (empty)"
log "STEP 1 pre-flight"; "$BIN" --cert-preflight; "$BIN" --scan-watch-selftest 2>&1 | grep -v "^objc"
log "STEP 2 scan + categorise (headless)"; T0=$(date +%s)
"$BIN" --ledger-root "$L" --scan --categorise 2>&1 | grep -vE "^objc\[" | tee "$C/scan.log" | grep -E "^SCAN|CATEGORISE|supervisor:|window watch" | cut -c1-200
W=$(( $(date +%s) - T0 ))
NL=$(python3 -c "import json;print(len(json.load(open('$L/licence-stops.json'))))" 2>/dev/null || echo 0)
NQ=$(python3 -c "import json;print(len(json.load(open('$L/quarantine.json'))))" 2>/dev/null || echo 0)
log "scan+categorise wall $W s; licence stops: $NL; quarantined: $NQ"
log "STEP 3 census"; "$BIN" --cert-sweep-census "$C/fixtures" --ejmap-ledger "$L" 2>&1 | tee "$C/census-step3.txt" | grep -E "worklist:|RUNNABLE:|QUARANTINED|NEEDS LICENCE|EMO-D5 \(s\)|crispytuner|CL 1B|APB C-18"
log "STEP 4 batch"; T1=$(date +%s)
caffeinate -i "$BIN" --cert-sweep-all --profile --ejmap-ledger "$L" --out "$C" --slice $S/rehearsal/slice.txt 2>&1 | tee "$C/batch.log" | grep -E "^=== |^  -> |^  wall|SWEEP-ALL|OUTCOMES|INVARIANT|^iLok" | cut -c1-200
log "batch wall $(( $(date +%s) - T1 )) s"
log "STEP 6 zip"; (cd $S/rehearsal && zip -rq $S/rehearsal/ejmap-cert-unbroken.zip cert_unbroken) && echo "config.json in zip: $(unzip -l $S/rehearsal/ejmap-cert-unbroken.zip | grep -c config.json)"
log "UNBROKEN DONE"
