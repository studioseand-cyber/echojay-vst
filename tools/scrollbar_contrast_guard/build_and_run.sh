#!/bin/bash
# scrollbar_contrast_guard (17 Sep 2026). EJ_LIB/EJ_SRC_ROOT/EJ_CXXFLAGS=-DEJ_GUARD_TODAY for the RED run.
ISOHOME="$(mktemp -d /tmp/echojay-scroll-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/scrollbar_contrast_guard/harness.cpp
