#!/bin/bash
# Build + run the workspace round-trip self-test against the real objects.
# Usage: ./build_and_run.sh   (from this directory; requires build/ to exist)
#
# EJ_SELFTEST compiles the test body into a FRESH EchoJayWorkspace.o (the
# release static lib carries only the stub); the rest links from the lib.
set -e
cd "$(dirname "$0")/../.."
SCRATCH=$(mktemp -d)
python3 - "$SCRATCH" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch = sys.argv[1]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('EchoJayWorkspace.cpp')][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('EchoJayWorkspace.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-DEJ_SELFTEST=1', '-I', os.path.abspath('Source'),
        'Source/EchoJayWorkspace.cpp', 'tools/workspace_roundtrip_test/rt_test.cpp',
        'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/rt_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode: print(r.stderr[-2000:]); sys.exit(1)
PYEOF
"$SCRATCH/rt_test"
