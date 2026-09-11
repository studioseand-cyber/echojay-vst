#!/bin/bash
# THE GATE BUILD (6 Sep 2026). Every target the gate exercises, by name, so a
# target nothing builds is no longer a target nothing tests. Kathy had to build
# the main plugin archive, both pitch harnesses and the device registry test BY
# HAND because no gate step named them; EchoJayBuiltinRegistryTest is
# EXCLUDE_FROM_ALL and is invisible to any build that does not say its name.
# NEVER a bare -j on this project (two swap-kills): -j 4, explicit targets, a log.
# Usage: gate_build.sh [BUILD_DIR] [LOG]     default build-release
set -uo pipefail
ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
BUILD_DIR="${1:-build-release}"; [[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$ROOT/$BUILD_DIR"
LOG="${2:-$BUILD_DIR/gate_build.log}"
TARGETS=(EchoJay EchoJay_AU EchoJay_VST3 EchoJayLink_AU EchoJayLink_VST3
         EchoJayPitchModeTest EchoJayPitchHostTest EchoJayBuiltinRegistryTest)
echo "gate build of $(git -C "$ROOT" rev-parse --short HEAD) in $BUILD_DIR, started $(date)" | tee "$LOG"
cmake --build "$BUILD_DIR" --target "${TARGETS[@]}" -j 4 >> "$LOG" 2>&1
rc=$?
echo "exit=$rc  errors=$(grep -c 'error:' "$LOG")  warnings=$(grep -c 'warning:' "$LOG")" | tee -a "$LOG"
exit $rc
