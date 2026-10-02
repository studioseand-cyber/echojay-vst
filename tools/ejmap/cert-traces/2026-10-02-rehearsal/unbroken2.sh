#!/bin/bash
# THE TEST, run 2 (2 Oct, after the ruling): ONE unbroken run from an EMPTY ledger and an EMPTY store, the stranger's
# runbook verbatim, with the harness's additions: scratch roots, the slice, and two labelled scratch edits after
# categorise (bx_crispytuner category pitch = stand-in for Sean's pending catalogue fix; Lindell 7X-500's server
# map-state entries removed so it is certified with NO map). Then the mapping step maps Lindell locally (nothing
# sent) and the census shows the local map as information.
S=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad
BIN="$S/dist/ejmap.app/Contents/MacOS/ejmap"
L=$S/rehearsal/ledger_empty; C=$S/rehearsal/cert_unbroken
log() { echo "[$(date +%T)] $*"; }
rm -rf "$L" "$C"; mkdir -p "$L" "$C"; cp ~/Library/ejmap/config.json "$L"/
log "UNBROKEN-2 START  ledger $L (empty but the sign-in token)  cert $C (empty)"
log "STEP 1 pre-flight"; "$BIN" --cert-preflight; "$BIN" --scan-watch-selftest 2>&1 | grep -v "^objc"
log "STEP 2 scan + categorise (headless)"; T0=$(date +%s)
"$BIN" --ledger-root "$L" --scan --categorise 2>&1 | grep -vE "^objc\[" | tee "$C/scan.log" | grep -E "^SCAN|CATEGORISE|supervisor: STOPPING" | cut -c1-200
W=$(( $(date +%s) - T0 ))
NL=$(python3 -c "import json;print(len(json.load(open('$L/licence-stops.json'))))" 2>/dev/null || echo 0)
NQ=$(python3 -c "import json;print(len(json.load(open('$L/quarantine.json'))))" 2>/dev/null || echo 0)
log "scan+categorise wall $W s; licence stops: $NL; quarantined: $NQ"
python3 - <<PY
import json
L="$L"
c=json.load(open(L+"/categories.json")); v=c["products"]["bx_crispytuner|plugin alliance"]
v["category"]="pitch"; v["stand_in"]="category pitch set by the test as a stand-in for Sean's pending catalogue fix (server: null / no_dial_set, 5 Aug); disposition left as served - scratch copy, 2 Oct"
json.dump(c,open(L+"/categories.json","w"))
ms=json.load(open(L+"/map-state.json")); ids=ms["identities"]; removed=[k for k in list(ids) if "6f457232" in k.lower()]
for k in removed: del ids[k]
ms["test_edit"]="Lindell 7X-500 server entries removed (%s): certified with NO map - scratch copy, 2 Oct"%removed
json.dump(ms,open(L+"/map-state.json","w"))
print("TEST EDITS: bx_crispytuner category pitch (stand-in); Lindell 7X-500 map-state removed", removed)
PY
log "STEP 3 census"; "$BIN" --cert-sweep-census "$C/fixtures" --ejmap-ledger "$L" 2>&1 | tee "$C/census-step3.txt" | grep -E "worklist:|RUNNABLE:|QUARANTINED|NEEDS LICENCE|NO MAP YET|pitch category|EMO-D5 \(s\)|crispytuner|7X-500|CL 1B|APB C-18" | cut -c1-250
log "STEP 4 batch"; T1=$(date +%s)
caffeinate -i "$BIN" --cert-sweep-all --profile --ejmap-ledger "$L" --out "$C" --slice $S/rehearsal/slice2.txt 2>&1 | tee "$C/batch.log" | grep -E "^=== |^  -> |^  wall|SWEEP-ALL|OUTCOMES|INVARIANT|^iLok" | grep -v "needs_licence: activation" | cut -c1-200
log "batch wall $(( $(date +%s) - T1 )) s"
log "STEP 3b (after, for the record): map Lindell 7X-500 locally, nothing sent"; T2=$(date +%s)
"$BIN" --ledger-root "$L" --sweep --resweep-targets $S/rehearsal/map_targets.txt 2>&1 | grep -vE "^objc\[" | tee "$C/mapping.log" | grep -E "RESWEEP|mapped  |declining|sent" | cut -c1-200
log "mapping wall $(( $(date +%s) - T2 )) s; local maps: $(ls $L/maps | wc -l)"
python3 - <<PY
import json
L="$L"
ms=json.load(open(L+"/map-state.json")); ids=ms["identities"]; removed=[k for k in list(ids) if "6f457232" in k.lower()]
for k in removed: del ids[k]
json.dump(ms,open(L+"/map-state.json","w")); print("map-state re-fetched by the sweep launch; Lindell server entries removed again:", removed)
PY
"$BIN" --cert-sweep-census "$C/fixtures" --ejmap-ledger "$L" 2>&1 | tee "$C/census-after-mapping.txt" | grep -E "7X-500|local map\(s\)" | cut -c1-200
log "STEP 6 zip"; (cd $S/rehearsal && rm -f ejmap-cert-unbroken2.zip && zip -rq ejmap-cert-unbroken2.zip cert_unbroken) && echo "config.json in zip: $(unzip -l $S/rehearsal/ejmap-cert-unbroken2.zip | grep -c config.json)"
log "UNBROKEN-2 DONE"
