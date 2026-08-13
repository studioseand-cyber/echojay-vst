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
