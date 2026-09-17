#!/bin/bash
ISOHOME="$(mktemp -d /tmp/echojay-budget-home.XXXXXX)"; export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
cd "$(dirname "$0")/../.."; exec python3 tools/harness_build.py tools/borrow_budget_guard/harness.cpp
