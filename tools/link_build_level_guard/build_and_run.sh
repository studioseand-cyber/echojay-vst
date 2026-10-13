#!/bin/bash
# link_build_level_guard (item 11, 13 Oct 2026): does a build on a Link arm that Link's level match?
# ONE process - a real LinkProcessor against the Link archive, with a real chain-cmd on disk, because the claim
# is about the Link's own build path. Isolated state root, as every harness here is.
set -u; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
S=${EJ_SCRATCH:-$(mktemp -d /tmp/echojay-guard-scratch.XXXXXX)}
ISO=$(mktemp -d /tmp/echojay-lblg-home.XXXXXX); export HOME=$ISO EJ_STATE_TEST_HOME=$ISO ECHOJAY_STATE_HOME=$ISO
H=$ISO/lblg; mkdir -p "$H"; echo "isolated home $ISO"
echo "-- compile link side"; rm -f "$S/lblg_bin"
bash tools/merge_gate_tests/compile_link_harness.sh tools/link_build_level_guard/link_side.cpp "$S/lblg_bin"
if [ ! -x "$S/lblg_bin" ]; then
  echo "  FAIL  the link side does not compile on this tree"
  echo "==== link_build_level_guard: RED ===="; rm -rf "$ISO"; exit 1
fi
"$S/lblg_bin" "$H" > "$H/link.log" 2>&1; LEXIT=$?
grep -E "^  (ok|FAIL)|^====|^  link uid|^  warm-up" "$H/link.log"
RC=1; grep -q "(link side): GREEN" "$H/link.log" && RC=0
[ $LEXIT -ne 0 ] && [ $RC -eq 0 ] && echo "  note: the link process exited $LEXIT after its verdict (headless LinkProcessor teardown)"
rm -rf "$ISO"
if [ $RC -eq 0 ]; then echo "==== link_build_level_guard: GREEN ===="; exit 0; else echo "==== link_build_level_guard: RED ===="; exit 1; fi
