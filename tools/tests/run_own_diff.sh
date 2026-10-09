#!/bin/bash
# own_diff is the one guard whose verdict is not its exit code: the binary writes the own-channel injection TEXT to
# /tmp/own_out.txt, and the CHECK is that text's sha256 against the baseline recorded in MERGE on 19 Sep 2026.
# Kept exactly as the suite script did it (shasum -a 256 /tmp/own_out.txt), only moved inside the test so ctest
# can hold the verdict. RULE, unchanged: if this goes RED, STOP AND REPORT - never re-baseline.
#
# 9 OCT 2026 - THE OUTPUT PATH IS NOW PER-CHECKOUT, and this is a RED this guard produced about itself. The
# harness takes the path as argv[1] and defaults to the hardcoded "/tmp/own_out.txt"; this wrapper never passed
# one. /tmp is shared by every checkout on the Mac, so while the 08c gate ran here, the limiter-v2 worktree's own
# own_diff ran in ~/echojay-limiter and overwrote the file between this binary writing it and this shasum reading
# it. The sha moved, the diff was entirely EedLimiterProcessor's v2 schema - parameters that do not exist in this
# tree at all - and the verdict was about the other checkout. ISOLATION IS A PRECONDITION FOR EVIDENCE, and
# run_guard.sh already provides the root: EJ_GUARD_HOME_BASE is handed down by CMake inside THIS build tree. The
# baseline is NOT touched, per the standing rule.
set -u
BASELINE="$1"; shift
OUT="${EJ_GUARD_HOME_BASE:-/tmp}/own_out.txt"
mkdir -p "$(dirname "$OUT")" 2>/dev/null
rm -f "$OUT"
"$(dirname "$0")/run_guard.sh" "$@" -- "$OUT"; RC=$?
[ $RC -eq 2 ] && exit 2
SHA="$(shasum -a 256 "$OUT" 2>/dev/null | awk '{print $1}')"
if [ -z "$SHA" ]; then echo "RUNNER REFUSED: own_diff wrote no text to $OUT"; exit 2; fi
echo "OWN_DIFF_OUT $OUT"
echo "OWN_DIFF_SHA $SHA"
echo "OWN_DIFF_BASELINE $BASELINE"
if [ "$SHA" = "$BASELINE" ]; then echo "own_diff: GREEN (sha matches the baseline)"; else echo "own_diff: RED (sha moved - STOP AND REPORT, do not re-baseline)"; RC=1; fi
exit $RC
