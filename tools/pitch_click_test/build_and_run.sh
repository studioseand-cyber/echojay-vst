#!/bin/bash
# Build and run the click-density test for EchoJay Pitch.
#
# WHY THIS TEST EXISTS. The device shipped 5.5 audible clicks a second on a
# real acapella while every existing test stayed green - pitch accuracy,
# nulls, block-size exactness, pluginval strictness 5. Only a listen caught
# it, and a listen does not run in CI. This runs the detector that DID catch
# it, with its own positive control (injected discontinuities of known size
# at known positions must be found), and asserts a click-density ceiling.
#
# MATERIAL. Real-vocal density is only measurable on a real vocal, and the
# repo does not name the author's library files (see PITCH_P0_VALIDATION.md
# on why). The path lives in an UNCOMMITTED sibling file:
#     tools/pitch_click_test/material.local     (git-ignored)
# containing one line: the absolute path to the test vocal. When present,
# the density gate runs on it. When absent, the test still runs the positive
# control on a synthetic vocal - weaker, but never silent.
set -e
cd "$(dirname "$0")/../.."

cmake --build build --target EchoJayPitchClickTest --config Debug 2>&1 | grep -E "error|warning: " || true
BIN=build/EchoJayPitchClickTest_artefacts/Debug/EchoJayPitchClickTest
[ -x "$BIN" ] || { echo "build failed"; exit 1; }

# The ceiling: measured 2.43/s on the real acapella after the emitDry-flip
# fix, against 5.48/s before it (and the pre-fix run showed the signature the
# ceiling exists to catch: 39% of clicks at a fixed 16-23 samples after a
# hop, hard amplitude steps at every tracking gap). The residual 2.43/s is
# wet-path roughness on sharp-glottal and fricative passages - small burrs,
# not steps; see PITCH_P0_VALIDATION.md §14. 3.5/s is a regression alarm,
# not a quality target: enough margin for material noise, and the original
# failure (5.5/s) is comfortably above it.
CEILING=3.5

MATERIAL_FILE="tools/pitch_click_test/material.local"
if [ -f "$MATERIAL_FILE" ]; then
    WAV="$(head -1 "$MATERIAL_FILE")"
    if [ -f "$WAV" ]; then
        echo "== real material: control + density gate (ceiling $CEILING/s) =="
        "$BIN" "$WAV" --control --max-per-second "$CEILING"
    else
        echo "material.local names a missing file: $WAV" >&2
        exit 1
    fi
else
    echo "== no material.local: positive control on synthetic vocal only =="
    echo "   (echo /path/to/vocal.wav > $MATERIAL_FILE to enable the real gate)"
    "$BIN" --synth --control
fi
echo "PASS"
