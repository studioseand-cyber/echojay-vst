#!/bin/bash
# Build + run the tab strip geometry self-test against the real objects.
# Usage: ./build_and_run.sh   (from anywhere; requires build/ to exist)
#
# Same shape as tools/workspace_roundtrip_test/build_and_run.sh: lift the real
# compile flags out of build/compile_commands.json so the test TU sees exactly
# the JUCE module defines and include paths the plugin is built with, then link
# the Release SharedCode static lib for the symbols.
#
# It links the lib rather than recompiling PluginEditor.cpp because
# computeTabRects and tabIndexIn are static members with no editor state, so
# the linker can hand them over without an instance ever existing.
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
        'tools/tabstrip_test/tabstrip_test.cpp',
        'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/tabstrip_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-3000:])
    sys.exit(1)
PYEOF
"$SCRATCH/tabstrip_test"
