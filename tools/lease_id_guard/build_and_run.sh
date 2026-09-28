#!/bin/bash
# lease_id_guard (22 Sep 2026, 21n item 2): the V2 side pushes an alias over the ctrl-cmd transport; the Link side (a real
# LinkProcessor + LinkEditor against the Link archive) shows it, keeps its identity, and clears it on Reset name.
# Two processes, as link_state_guard: EJ_LINK_LIB / EJ_LIB + EJ_SRC_ROOT pair a RED run with the pre-round archives + headers.
set -u; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
S=${EJ_SCRATCH:-$(mktemp -d /tmp/echojay-guard-scratch.XXXXXX)}   # 21t-j: no session path baked in
ISO=$(mktemp -d /tmp/echojay-lig-home.XXXXXX); export HOME=$ISO EJ_STATE_TEST_HOME=$ISO ECHOJAY_STATE_HOME=$ISO
H=$ISO/lig; mkdir -p $H; echo "isolated home $ISO"
echo "-- compile link side"; LOK=0; rm -f $S/lig_link_bin; bash tools/merge_gate_tests/compile_link_harness.sh tools/lease_id_guard/link_side.cpp $S/lig_link_bin 2>&1 | grep -E "error:|compiled" | head -5; [ -x $S/lig_link_bin ] && LOK=1
echo "-- compile v2 side"; V2OUT=$(python3 tools/harness_build.py tools/lease_id_guard/v2_side.cpp 2>&1 | tee /dev/stderr | grep -E "^compiled -> " | tail -1 | sed 's/^compiled -> //')
V2BIN="$V2OUT"; V2OK=0; [ -n "$V2BIN" ] && [ -x "$V2BIN" ] && V2OK=1   # 21t-j: the path the builder printed, not an assumed one
# 28 Sep 2026: ...AND IT MUST BE THIS GUARD'S OWN BINARY. Five guards have a v2_side.cpp and they all
# compiled to one path, so this runner once launched another guard's V2 side and read its silence as a
# product failure. The name is now unique; this line makes a future collision stop the run instead.
case "$V2BIN" in *lease_id_guard*) : ;; *) echo "  FAIL  the compiled v2 binary is not this guard's: $V2BIN"; V2OK=0 ;; esac
if [ $LOK = 0 ]; then echo "  FAIL  link side does not compile on this tree ((link side)) - RED by construction"; echo "==== lease_id_guard: RED ===="; exit 1; fi
$S/lig_link_bin $H > $H/link.log 2>&1 & LP=$!
for i in $(seq 1 120); do [ -f $H/link_ready.json ] && break; sleep 0.5; done
if [ $V2OK = 1 ]; then EJ_LIG_HOME=$H $V2BIN > $H/v2.log 2>&1; echo "v2 side rc=$?"; else echo "  FAIL  v2 side did not compile"; fi
wait $LP; LEXIT=$?
echo "== link side =="; grep -E "^  (ok|FAIL)|^====|link side:" $H/link.log
[ $V2OK = 1 ] && { echo "== v2 side =="; grep -E "^  (ok|FAIL)|^====" $H/v2.log; }
LRC=1; grep -q "(link side): GREEN" $H/link.log && LRC=0
V2RC=1; [ $V2OK = 1 ] && grep -q "(v2 side): GREEN" $H/v2.log && V2RC=0
[ $LEXIT -ne 0 ] && echo "  note: link side process exit $LEXIT after its verdict (headless LinkProcessor teardown)"
if [ $LRC -eq 0 ] && [ $V2RC -eq 0 ]; then echo "==== lease_id_guard: GREEN ===="; exit 0; else echo "==== lease_id_guard: RED ===="; exit 1; fi
