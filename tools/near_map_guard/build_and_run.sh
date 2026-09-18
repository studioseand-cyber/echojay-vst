#!/bin/bash
# near_map_guard - hurdle 1 items 2 + 3 (17 Sep 2026). EJ_LIB=<lib> EJ_CXXFLAGS=-DEJ_GUARD_TODAY for the RED run on the pre-round lib.
ISOHOME="$(mktemp -d /tmp/echojay-nearmap-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; export EJ_SCRIBBLE_LEG=1   # 18 Sep 2026: plain run, then MallocScribble/PreScribble/GuardEdges; fail if either fails
exec python3 tools/harness_build.py tools/near_map_guard/harness.cpp
