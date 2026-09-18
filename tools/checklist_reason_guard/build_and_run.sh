#!/bin/bash
# checklist_reason_guard (18 Sep 2026). EJ_LIB/EJ_SRC_ROOT/EJ_CXXFLAGS=-DEJ_GUARD_TODAY for the RED run.
ISOHOME="$(mktemp -d /tmp/echojay-checklist-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/checklist_reason_guard/harness.cpp
