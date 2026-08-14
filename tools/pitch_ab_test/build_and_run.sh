#!/bin/bash
# Build and run the Antares-anchored quality gate for EchoJay Pitch.
#
# WHY. An independent A/B against Antares Auto-Tune Pro found the device
# losing on cents-from-note, HNR and spectral flux while every existing test
# stayed green (PITCH_P0_VALIDATION.md §16). This gate re-measures those
# three against the ANTARES bounce - fresh, not transcribed - on a render
# made from the current source by g++, so it can never test a stale binary.
#
# MATERIAL. A folder holding dry.wav / echojay.wav / antares.wav bounces of
# the same take (the reference session: low_male, D chromatic, retune 0,
# hard-matched). The path lives in the UNCOMMITTED sibling file:
#     tools/pitch_ab_test/material.local     (git-ignored)
# When absent, the gate explains itself and exits 0 - visible, never silent.
set -e
cd "$(dirname "$0")/../.."

g++ -std=c++17 -O2 -ISource tools/pitch_ab_test/main.cpp -o /tmp/echojay_pitch_ab_test

MATERIAL_FILE="tools/pitch_ab_test/material.local"
if [ -f "$MATERIAL_FILE" ]; then
    DIR="$(head -1 "$MATERIAL_FILE")"
    if [ -d "$DIR" ]; then
        /tmp/echojay_pitch_ab_test "$DIR"
    else
        echo "material.local names a missing folder: $DIR" >&2
        exit 1
    fi
else
    echo "== no material.local: the Antares-anchored gate needs the reference bounces =="
    echo "   (echo /path/to/bounce-folder > $MATERIAL_FILE to enable it)"
fi
