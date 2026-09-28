#!/bin/bash
# level_match_guard (21t-j, 28 Sep 2026): the Apply path, end to end, in TWO PROCESSES - three real LinkProcessors
# against the Link archive and a real EchoJayProcessor + editor against V2's. Sean's test 4 read "6 of 6 channels
# took the change" while every Link was answering "ABORTED applied=0/1" with an ack of "stale", so a registry
# fixture cannot be the guard for it: only a real Link can refuse, ack, move its own trim and publish the frame the
# record is taken from.
#   (1) each Link's own trim moves by its member's delta   (2) an aborted rack edit acks not_applied, with a reason
#   (3) the card's sentence counts the READBACK             (4) the record and the strip agree within 0.2 dB
set -u; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
S=${EJ_SCRATCH:-$(mktemp -d /tmp/echojay-guard-scratch.XXXXXX)}   # 21t-j: no session path baked in
ISO=$(mktemp -d /tmp/echojay-lmg-home.XXXXXX); export HOME=$ISO EJ_STATE_TEST_HOME=$ISO ECHOJAY_STATE_HOME=$ISO
H=$ISO/lmg; mkdir -p $H; echo "isolated home $ISO"
echo "-- compile link side"; LOK=0; rm -f $S/lmg_link_bin
bash tools/merge_gate_tests/compile_link_harness.sh tools/level_match_guard/link_side.cpp $S/lmg_link_bin 2>&1 | grep -E "error:|compiled" | head -6
[ -x $S/lmg_link_bin ] && LOK=1
echo "-- compile v2 side"; V2OUT=$(python3 tools/harness_build.py tools/level_match_guard/v2_side.cpp 2>&1 | tee /dev/stderr | grep -E "^compiled -> " | tail -1 | sed 's/^compiled -> //')
V2BIN="$V2OUT"; V2OK=0; [ -n "$V2BIN" ] && [ -x "$V2BIN" ] && V2OK=1   # 21t-j: the path the builder printed, not an assumed one
# 28 Sep 2026: ...AND IT MUST BE THIS GUARD'S OWN BINARY. Five guards have a v2_side.cpp and they all
# compiled to one path, so this runner once launched another guard's V2 side and read its silence as a
# product failure. The name is now unique; this line makes a future collision stop the run instead.
case "$V2BIN" in *level_match_guard*) : ;; *) echo "  FAIL  the compiled v2 binary is not this guard's: $V2BIN"; V2OK=0 ;; esac
if [ $LOK = 0 ]; then echo "  FAIL  link side does not compile on this tree - RED by construction"; echo "==== level_match_guard: RED ===="; exit 1; fi
if [ $V2OK = 0 ]; then echo "  FAIL  v2 side does not compile on this tree - RED by construction"; echo "==== level_match_guard: RED ===="; exit 1; fi
$S/lmg_link_bin $H > $H/link.log 2>&1 & LP=$!
for i in $(seq 1 240); do [ -f $H/link_ready.json ] && break; sleep 0.5; done
EJ_LMG_HOME=$H $V2BIN > $H/v2.log 2>&1; V2EXIT=$?
echo "v2 side rc=$V2EXIT"
wait $LP; LEXIT=$?
echo "== link side =="; grep -E "^  (ok|FAIL)|^====|^    |link side:" $H/link.log
echo "== v2 side ==";   grep -E "^  (ok|FAIL)|^====|^    " $H/v2.log
LRC=1; grep -q "(link side): GREEN" $H/link.log && LRC=0
V2RC=1; grep -q "(v2 side): GREEN" $H/v2.log && V2RC=0
[ $LEXIT -ne 0 ] && echo "  note: link side process exit $LEXIT after its verdict (headless LinkProcessor teardown)"
if [ $LRC -eq 0 ] && [ $V2RC -eq 0 ]; then echo "==== level_match_guard: GREEN ===="; exit 0; else echo "==== level_match_guard: RED ===="; exit 1; fi
