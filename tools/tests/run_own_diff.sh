#!/bin/bash
# own_diff is the one guard whose verdict is not its exit code: the binary writes the own-channel injection TEXT to
# /tmp/own_out.txt, and the CHECK is that text's sha256 against the baseline recorded in MERGE on 19 Sep 2026.
# Kept exactly as the suite script did it (shasum -a 256 /tmp/own_out.txt), only moved inside the test so ctest
# can hold the verdict. RULE, unchanged: if this goes RED, STOP AND REPORT - never re-baseline.
set -u
BASELINE="$1"; shift
rm -f /tmp/own_out.txt
"$(dirname "$0")/run_guard.sh" "$@"; RC=$?
[ $RC -eq 2 ] && exit 2
SHA="$(shasum -a 256 /tmp/own_out.txt 2>/dev/null | awk '{print $1}')"
echo "OWN_DIFF_SHA $SHA"
echo "OWN_DIFF_BASELINE $BASELINE"
if [ "$SHA" = "$BASELINE" ]; then echo "own_diff: GREEN (sha matches the baseline)"; else echo "own_diff: RED (sha moved - STOP AND REPORT, do not re-baseline)"; RC=1; fi
exit $RC
