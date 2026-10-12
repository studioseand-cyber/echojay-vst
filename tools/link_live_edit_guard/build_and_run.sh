#!/bin/bash
# link_live_edit_guard (11 Oct 2026): Sean's in-host report, as a leg. A V2 addressing a Link builds a +12 dB
# gain slot into it and then changes that slot's gain, and the LINK's RENDERED OUTPUT has to move both times,
# with no rack switch anywhere. Two processes, both real, because the bug lived in the gap between them.
set -u; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
S=${EJ_SCRATCH:-$(mktemp -d /tmp/echojay-guard-scratch.XXXXXX)}
ISO=$(mktemp -d /tmp/echojay-lle-home.XXXXXX); export HOME=$ISO EJ_STATE_TEST_HOME=$ISO ECHOJAY_STATE_HOME=$ISO
H=$ISO/lle; mkdir -p "$H"; echo "isolated home $ISO"
echo "-- compile link side"; LOK=0; rm -f $S/lle_link_bin
bash tools/merge_gate_tests/compile_link_harness.sh tools/link_live_edit_guard/link_side.cpp $S/lle_link_bin 2>&1 | grep -E "error:|compiled" | head -8
[ -x $S/lle_link_bin ] && LOK=1
echo "-- compile v2 side"; V2OUT=$(python3 tools/harness_build.py tools/link_live_edit_guard/v2_side.cpp 2>&1 | tee /dev/stderr | grep -E "^compiled -> " | tail -1 | sed 's/^compiled -> //')
V2BIN="$V2OUT"; V2OK=0; [ -n "$V2BIN" ] && [ -x "$V2BIN" ] && V2OK=1
# The level_match_guard lesson: five guards have a v2_side.cpp. Make a collision stop the run.
case "$V2BIN" in *link_live_edit_guard*) : ;; *) echo "  FAIL  the compiled v2 binary is not this guard's: $V2BIN"; V2OK=0 ;; esac
if [ $LOK = 0 ];  then echo "  FAIL  link side does not compile on this tree - RED by construction"; echo "==== link_live_edit_guard: RED ===="; rm -rf "$ISO"; exit 1; fi
if [ $V2OK = 0 ]; then echo "  FAIL  v2 side does not compile on this tree - RED by construction";  echo "==== link_live_edit_guard: RED ===="; rm -rf "$ISO"; exit 1; fi
$S/lle_link_bin "$H" > "$H/link.log" 2>&1 & LP=$!
for i in $(seq 1 240); do [ -f "$H/link_ready.json" ] && break; sleep 0.5; done
EJ_LLE_HOME=$H $V2BIN > "$H/v2.log" 2>&1; V2EXIT=$?
wait $LP; LEXIT=$?
echo "== link side =="; grep -E "^  (ok|FAIL)|^====|^    |link side:" "$H/link.log"
echo "== v2 side ==";   grep -E "^  (ok|FAIL)|^====|^    |v2 side:" "$H/v2.log"
LRC=1; grep -q "(link side): GREEN" "$H/link.log" && LRC=0
V2RC=1; grep -q "(v2 side): GREEN" "$H/v2.log" && V2RC=0
[ $LEXIT -ne 0 ] && echo "  note: link side process exit $LEXIT after its verdict (headless LinkProcessor teardown)"
rm -rf "$ISO"
if [ $LRC -eq 0 ] && [ $V2RC -eq 0 ]; then echo "==== link_live_edit_guard: GREEN ===="; exit 0; else echo "==== link_live_edit_guard: RED ===="; exit 1; fi
