#!/bin/bash
# slotwet_knobpath: COMMIT 3b guard — the REAL V2 knob path on a HELD Link-rack
# slot writes the BorrowHost (heard) AND the slotWet verb. V2 lib via
# tools/harness_build.py. Isolation: private HOME/EJ_STATE_TEST_HOME here;
# ECHOJAY_STATE_HOME inside the harness.
ISOHOME="$(mktemp -d /tmp/echojay-knobpath-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
echo "isolated HOME: $ISOHOME"
cd "$(dirname "$0")/../.."
exec python3 tools/harness_build.py tools/slotwet_knobpath/harness.cpp
