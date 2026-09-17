#!/bin/bash
ISOHOME="$(mktemp -d /tmp/echojay-ovstep0-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/overlay_step0/harness.cpp
