#!/bin/bash
# preflight_guard (17 Sep 2026). EJ_LIB/EJ_SRC_ROOT/EJ_CXXFLAGS=-DEJ_GUARD_TODAY for the RED run.
ISOHOME="$(mktemp -d /tmp/echojay-preflight-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; export EJ_SCRIBBLE_LEG=1   # 18 Sep 2026: plain run, then MallocScribble/PreScribble/GuardEdges; fail if either fails
exec python3 tools/harness_build.py tools/preflight_guard/harness.cpp
