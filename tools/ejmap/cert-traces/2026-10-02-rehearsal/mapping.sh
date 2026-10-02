#!/bin/bash
# ITEM 3 (2 Oct): MAPPING IN THE LOOP. After the unbroken test ends: map two products LOCALLY with EJ Map's mapping sweep
# (--resweep-targets: bx_crispytuner, and Lindell 7X-500 - every categorised compressor on this Mac already has a server
# map, so a mapped one is re-mapped locally to exercise the local-map hand-off), NOTHING SENT; then the census must show
# them "mapped (local map)"; then the batch resumed with the slice extended certifies from the local maps.
S=/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/df7309b6-cabf-4dc3-abd7-0db4d0956dc6/scratchpad
BIN="$S/dist/ejmap.app/Contents/MacOS/ejmap"
L=$S/rehearsal/ledger_empty; C=$S/rehearsal/cert_unbroken
log() { echo "[$(date +%T)] $*"; }
while ! grep -q "UNBROKEN DONE" $S/rehearsal/unbroken.log; do sleep 20; done
# TEST EDIT (a), BEFORE the mapping sweep: bx_crispytuner's category pitch - a labelled stand-in for Sean's pending catalogue
# fix; its disposition stays as the server has it (no_dial_set), so whether the mapping sweep maps it is MEASURED.
python3 - <<'PY'
import json
L="'"$L"'"
c=json.load(open(L+"/categories.json")); v=c["products"]["bx_crispytuner|plugin alliance"]
v["category"]="pitch"; v["stand_in"]="category pitch set by the test as a stand-in for Sean's pending catalogue fix (server says null / no_dial_set, 5 Aug); disposition left as served - scratch copy, 2 Oct"
json.dump(c,open(L+"/categories.json","w")); print("TEST EDIT (a): bx_crispytuner category pitch (stand-in), disposition", v.get("disposition"))
PY
log "MAPPING START  maps before: $(ls $L/maps | wc -l)"
T0=$(date +%s)
"$BIN" --ledger-root "$L" --sweep --resweep-targets $S/rehearsal/map_targets.txt 2>&1 | grep -vE "^objc\[" | tee "$C/mapping.log" | grep -E "RESWEEP|SWEEP|mapped|MAP:|sent|BACKLOG|supervisor:" | cut -c1-200
log "mapping wall $(( $(date +%s) - T0 )) s; maps after: $(ls $L/maps | wc -l); sent: $(grep -ci 'sent [1-9]' $C/mapping.log)"
ls -la $L/maps | tail -3
# THE TEST'S TWO SCRATCH EDITS (Kathy, 2 Oct), AFTER the mapping sweep (it re-fetches map-state at launch) and BEFORE the
# census and the batch (they read the files): (a) Lindell 7X-500's SERVER map-state entries removed, so its local map is the
# only way it can be discovered; (b) bx_crispytuner given category pitch - ONE product, a labelled stand-in for Sean's
# pending catalogue fix. Scratch copies only; the real ledger is untouched.
python3 - <<'PY'
import json
L="'"$L"'"
ms=json.load(open(L+"/map-state.json")); ids=ms["identities"]
lind=[k for k in ids if k.lower().startswith("audiounit|6f457232|") or k.lower().startswith("vst3|") and False]
# resolve Lindell 7X-500's identities by uid from the scan: AudioUnit|6f457232 (the record's uid)
removed=[k for k in list(ids) if "6f457232" in k.lower()]
for k in removed: del ids[k]
ms["test_edit"]="Lindell 7X-500 server entries removed (%s) so its LOCAL map is the only route to discovery - scratch copy, 2 Oct"%removed
json.dump(ms,open(L+"/map-state.json","w"))
print("TEST EDIT (b): removed map-state",removed)
PY
log "census after mapping"; "$BIN" --cert-sweep-census "$C/fixtures" --ejmap-ledger "$L" 2>&1 | tee "$C/census-after-mapping.txt" | grep -E "RUNNABLE:|local map|crispytuner|7X-500|UNMAPPED" | cut -c1-200
log "batch resumed with the slice extended"; T1=$(date +%s)
caffeinate -i "$BIN" --cert-sweep-all --profile --ejmap-ledger "$L" --out "$C" --slice $S/rehearsal/slice2.txt 2>&1 | tee "$C/batch-after-mapping.log" | grep -E "^=== |^  -> |^  wall|SWEEP-ALL|OUTCOMES|INVARIANT" | grep -v needs_licence | cut -c1-200
log "batch wall $(( $(date +%s) - T1 )) s"
(cd $S/rehearsal && rm -f ejmap-cert-unbroken.zip && zip -rq ejmap-cert-unbroken.zip cert_unbroken) && echo "config.json in zip: $(unzip -l $S/rehearsal/ejmap-cert-unbroken.zip | grep -c config.json)"
log "MAPPING DONE"
