#!/bin/bash
# role_snapshot_guard (21t-j, 28 Sep 2026): REGISTRATION IS A FULL SNAPSHOT, in TWO PROCESSES. Two real
# LinkProcessors against the Link's shipping archive - one set through the picker, one handed the other's saved
# state the way a host hands a COPIED INSERT - and a real EchoJayProcessor against V2's, reading the rows.
# Sean's defect: the copy arrived at V2 with no role at all. A registry fixture cannot be the guard for it,
# because the thing under test is what a real Link WRITES at registration and on every heartbeat.
set -u; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
S=${EJ_SCRATCH:-$(mktemp -d /tmp/echojay-guard-scratch.XXXXXX)}
ISO=$(mktemp -d /tmp/echojay-rsg-home.XXXXXX); export HOME=$ISO EJ_STATE_TEST_HOME=$ISO ECHOJAY_STATE_HOME=$ISO
H=$ISO/rsg; mkdir -p $H; echo "isolated home $ISO"
echo "-- compile link side"; LOK=0; rm -f $S/rsg_link_bin
bash tools/merge_gate_tests/compile_link_harness.sh tools/role_snapshot_guard/link_side.cpp $S/rsg_link_bin 2>&1 | grep -E "error:|compiled" | head -6
[ -x $S/rsg_link_bin ] && LOK=1
echo "-- compile v2 side"; V2OUT=$(python3 tools/harness_build.py tools/role_snapshot_guard/v2_side.cpp 2>&1 | tee /dev/stderr | grep -E "^compiled -> " | tail -1 | sed 's/^compiled -> //')
V2BIN="$V2OUT"; V2OK=0; [ -n "$V2BIN" ] && [ -x "$V2BIN" ] && V2OK=1
# 28 Sep 2026: ...AND IT MUST BE THIS GUARD'S OWN BINARY. Five guards have a v2_side.cpp and they all
# compiled to one path, so a runner once launched another guard's V2 side and read its silence as a
# product failure. The name is now unique; this line makes a future collision stop the run instead.
case "$V2BIN" in *role_snapshot_guard*) : ;; *) echo "  FAIL  the compiled v2 binary is not this guard's: $V2BIN"; V2OK=0 ;; esac
if [ $LOK = 0 ]; then echo "  FAIL  link side does not compile on this tree - RED by construction"; echo "==== role_snapshot_guard: RED ===="; exit 1; fi
if [ $V2OK = 0 ]; then echo "  FAIL  v2 side does not compile on this tree - RED by construction"; echo "==== role_snapshot_guard: RED ===="; exit 1; fi
$S/rsg_link_bin $H > $H/link.log 2>&1 & LP=$!
for i in $(seq 1 240); do [ -f $H/link_ready.json ] && break; sleep 0.5; done
EJ_RSG_HOME=$H $V2BIN > $H/v2.log 2>&1; V2EXIT=$?
echo "v2 side rc=$V2EXIT"
wait $LP; LEXIT=$?
echo "== link side =="; grep -E "^  (ok|FAIL)|^====|^    |link side:" $H/link.log
echo "== v2 side ==";   grep -E "^  (ok|FAIL)|^====|^    " $H/v2.log
LRC=1; grep -q "(link side): GREEN" $H/link.log && LRC=0
V2RC=1; grep -q "(v2 side): GREEN" $H/v2.log && V2RC=0
[ $LEXIT -ne 0 ] && echo "  note: link side process exit $LEXIT after its verdict (headless LinkProcessor teardown)"
if [ $LRC -eq 0 ] && [ $V2RC -eq 0 ]; then echo "==== role_snapshot_guard: GREEN ===="; exit 0; else echo "==== role_snapshot_guard: RED ===="; exit 1; fi
