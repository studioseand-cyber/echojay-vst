#!/bin/bash
ISOHOME="$(mktemp -d /tmp/echojay-uig-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; export EJ_SCRIBBLE_LEG=1   # 18 Sep 2026: plain run, then MallocScribble/PreScribble/GuardEdges; fail if either fails
exec python3 tools/harness_build.py tools/ui_round_guard/ui_guard.cpp
