#!/bin/bash
ISOHOME="$(mktemp -d /tmp/echojay-loudloop-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
export EJ_SCRIBBLE_LEG=1   # ruling G: scribble leg
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/loudness_loop_guard/harness.cpp
