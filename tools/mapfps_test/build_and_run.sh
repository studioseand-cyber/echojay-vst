#!/bin/bash
# Build + run the fpForIdentity join self-test against the real header.
# Usage: ./build_and_run.sh   (from anywhere; requires build/ to exist)
#
# Same shape as tools/paramapply_test: lift the real compile flags out of
# build/compile_commands.json so the test TU sees exactly the JUCE module
# defines and include paths the plugin is built with, then link the Release
# SharedCode static lib for the JUCE symbols.
#
# EchoJayParamMaps.h is header-only inline code, so the test TU compiles the
# SHIPPED implementation directly - there is no lib copy of these functions
# to drift from. The test also reads Source/ChainHost.cpp from the repo root
# (structural wiring check), so it must run with cwd at the repo root, which
# the cd below guarantees.
set -e
cd "$(dirname "$0")/../.."

# ---- THE LINK COMPILE GATE (5 Sep 2026) ----------------------------------
# WHY THIS EXISTS. On 5 September a required LoadOrigin argument was added to
# ChainHost::loadPluginAsync. The main SharedCode archive built clean and this
# gate went green at 654 ok through six mutations, while LinkProcessor.cpp did
# not compile at all: a lambda driving the Link's per-slot loads had not
# captured the new parameter. The defect surfaced only at the FORMAT build, on
# EchoJayLink_AU, which is the last step before installing.
#
# The reason the gate could not see it is structural, not an oversight. The
# archive linked below is the MAIN plugin's translation units; LinkProcessor.cpp
# and LinkEditor.cpp are in a separate juce_add_plugin target (CMakeLists.txt
# :454) with its own SharedCode archive, and nothing in this harness compiled
# them. Any change to a ChainHost signature the Link calls had the same blind
# spot.
#
# COMPILE ONLY, DELIBERATELY. The two archives are NOT linked together: both
# plugins define JUCE plugin-entry symbols and a createPluginFilter, so a shared
# link is a duplicate-symbol problem to solve for no extra coverage. Building
# the Link's archive proves its translation units still compile against the
# headers this commit changed, which is the whole of what was missing. No pins
# are added here and none are wanted: the compile IS the assertion.
#
# IT STOPS THE GATE. This is a script, not something pasted into a shell, so a
# hard exit is correct: a warning that scrolled past would leave the same hole
# it is closing. set -e is already on, and the explicit block is here so the
# failure says what it was rather than trailing a wall of compiler output.
#
# It costs nothing when the Link is untouched: make rebuilds only what changed,
# so an unrelated commit sees "Built target EchoJayLink" and moves on. It does
# NOT build the AU or VST3 bundles, so an installed plugin is never disturbed
# by running the gate.
echo "== gate step 1/2: the Link's SharedCode archive must compile =="
if ! ( cd build && make -j"$(sysctl -n hw.ncpu)" EchoJayLink ); then
    echo "" >&2
    echo "GATE FAILED: EchoJayLink did not compile." >&2
    echo "The Link's translation units (LinkProcessor.cpp, LinkEditor.cpp) are" >&2
    echo "outside the main archive this harness links, so this step is the only" >&2
    echo "thing that compiles them. Fix the Link before the pins are meaningful." >&2
    exit 1
fi
echo "== gate step 2/2: mapfps_test =="

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
python3 - "$SCRATCH" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch = sys.argv[1]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('PluginEditor.cpp')][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('PluginEditor.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'),
        'tools/mapfps_test/mapfps_test.cpp',
        'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/mapfps_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-3000:])
    sys.exit(1)
PYEOF
"$SCRATCH/mapfps_test"
