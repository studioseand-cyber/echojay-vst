#!/bin/bash
# calib_link_guard (21t-d, 25 Sep 2026): does the LINK drive the calibration loop once it owns the rack?
# ONE process - a real LinkProcessor against the Link archive, with real audio through processBlock so the slot's
# own tallies close real 3 s windows. A stand-in writing sidecar state would prove nothing about the Link.
set -u; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
S=${EJ_SCRATCH:-$(mktemp -d /tmp/echojay-guard-scratch.XXXXXX)}   # 21t-j: no session path baked in
ISO=$(mktemp -d /tmp/echojay-calib-home.XXXXXX); export HOME=$ISO EJ_STATE_TEST_HOME=$ISO ECHOJAY_STATE_HOME=$ISO
H=$ISO/calib; mkdir -p "$H"; echo "isolated home $ISO"
echo "-- compile link side"; rm -f $S/calib_link_bin
bash tools/merge_gate_tests/compile_link_harness.sh tools/calib_link_guard/link_side.cpp $S/calib_link_bin
if [ ! -x $S/calib_link_bin ]; then
  echo "  FAIL  the link side does not compile on this tree (LinkProcessor has no calibration loop) - RED by construction"
  echo "==== calib_link_guard: RED ===="; rm -rf "$ISO"; exit 1
fi
$S/calib_link_bin "$H" > "$H/link.log" 2>&1; LEXIT=$?
grep -E "^  (ok|FAIL)|^====|^link side:" "$H/link.log"
RC=1; grep -q "(link side): GREEN" "$H/link.log" && RC=0
[ $LEXIT -ne 0 ] && [ $RC -eq 0 ] && echo "  note: the link process exited $LEXIT after its verdict (headless LinkProcessor teardown)"
rm -rf "$ISO"
if [ $RC -eq 0 ]; then echo "==== calib_link_guard: GREEN ===="; exit 0; else echo "==== calib_link_guard: RED ===="; exit 1; fi
