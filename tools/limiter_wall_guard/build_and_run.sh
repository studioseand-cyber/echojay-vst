#!/bin/bash
ISOHOME="$(mktemp -d /tmp/echojay-limwall-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
export EJ_SCRIBBLE_LEG=1
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/limiter_wall_guard/harness.cpp
