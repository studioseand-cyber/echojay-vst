#!/bin/bash
# builtin_registry_test, IN THE GATE (21t-m, 29 Sep 2026).
#
# The test itself is tools/builtin_registry_test.cpp and its target (EchoJayBuiltinRegistryTest) is declared
# EXCLUDE_FROM_ALL in the ROOT CMakeLists, so an ordinary plugin build does not pay for it. It had no ctest
# label at all, which breaks the standing rule that every test in the repo is either in the gate or carries a
# written exclusion. What that cost: its hard-coded device count read 22 while the product registers 23 - Level
# was added to every target's source list and to this test's, but not to its count - and the only binary of it
# on this machine was dated 21 August, so nothing had run it in five weeks.
#
# This builds the target FROM SOURCE every run and then runs it, so a stale binary can never be the answer.
set -u
cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
LOG=${EJ_SCRATCH:-$(mktemp -d /tmp/echojay-brg.XXXXXX)}/brg_build.log
echo "-- build EchoJayBuiltinRegistryTest (root tree, EXCLUDE_FROM_ALL)"
cmake --build build -j 4 --target EchoJayBuiltinRegistryTest > "$LOG" 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    echo "  FAIL  the target does not build on this tree - RED by construction"
    grep -E "error:" "$LOG" | sort -u | head -8
    echo "==== builtin_registry_test: RED ===="
    exit 1
fi
BIN=$(ls -t build/EchoJayBuiltinRegistryTest_artefacts/*/EchoJayBuiltinRegistryTest 2>/dev/null | head -1)
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "  FAIL  built, but no binary at build/EchoJayBuiltinRegistryTest_artefacts/*/"
    echo "==== builtin_registry_test: RED ===="
    exit 1
fi
echo "-- run $BIN"
# Isolated HOME and state root, like every other guard: this one reads no live config, and that is a fact the
# seal keeps true rather than a claim.
ISO=$(mktemp -d /tmp/echojay-brg-home.XXXXXX)
HOME=$ISO EJ_STATE_TEST_HOME=$ISO ECHOJAY_STATE_HOME=$ISO "$BIN"
RC=$?
if [ $RC -eq 0 ]; then echo "==== builtin_registry_test: GREEN ===="; else echo "==== builtin_registry_test: RED ===="; fi
exit $RC
