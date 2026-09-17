#!/bin/bash
ISOHOME="$(mktemp -d /tmp/echojay-hist-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/ui_round_guard/history_guard.cpp
