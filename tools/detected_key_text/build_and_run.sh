#!/bin/bash
# detected_key_text: prints the assembled [DETECTED KEY] block OFF/ON (COMMIT 4 proof).
# Compiles via tools/harness_build.py against the CURRENT V2 SharedCode lib.
# Isolation: private HOME/EJ_STATE_TEST_HOME here; ECHOJAY_STATE_HOME inside the harness.
ISOHOME="$(mktemp -d /tmp/echojay-keytext-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
echo "isolated HOME: $ISOHOME"
cd "$(dirname "$0")/../.."
exec python3 tools/harness_build.py tools/detected_key_text/harness.cpp
