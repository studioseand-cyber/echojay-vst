#!/bin/bash
# dialinfo_keep_guard (17 Sep 2026). EJ_LIB=<lib> EJ_SRC_ROOT=<worktree> EJ_CXXFLAGS=-DEJ_GUARD_TODAY for the RED run on the pre-round tree.
ISOHOME="$(mktemp -d /tmp/echojay-dialinfo_keep_guard-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/dialinfo_keep_guard/harness.cpp
