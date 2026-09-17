#!/bin/bash
# wet_rebuild_guard: COMMIT 5 guard on the real chain panel (V2 lib via harness_build.py).
ISOHOME="$(mktemp -d /tmp/echojay-wetguard-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/wet_rebuild_guard/harness.cpp
