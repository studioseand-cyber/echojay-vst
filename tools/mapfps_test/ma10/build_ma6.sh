#!/bin/bash
# MA6 (round 8h): build the capture-length harness and run it on the stems.
#
# usage: tools/mapfps_test/ma10/build_ma6.sh "<stem folder>"
#   BUILD_DIR   the configured build tree to lift flags and the SharedCode lib
#               from (default: build), as build_and_run.sh does.
#
# The same build as build_and_run.sh, on ma6_prefix.cpp. The binary and its
# output go in tools/mapfps_test/ma10/work/. The stems are only ever opened for
# reading.
set -e
cd "$(dirname "$0")/../../.."
BUILD_DIR="${BUILD_DIR:-build}"
STEMS="$1"
[ -n "$STEMS" ] || { echo "usage: $0 <stem folder>" >&2; exit 2; }
WORK="$(pwd)/tools/mapfps_test/ma10/work"
mkdir -p "$WORK"

python3 - "$BUILD_DIR" "$WORK" <<'PYEOF'
import json, sys, shlex, subprocess, os
bd, work = sys.argv[1], sys.argv[2]
cc = json.load(open(os.path.join(bd, 'compile_commands.json')))
entry = [e for e in cc if e['file'].endswith('PluginEditor.cpp')][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('PluginEditor.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'),
        'tools/mapfps_test/ma10/ma6_prefix.cpp',
        os.path.join(bd, 'EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a'),
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', os.path.join(work, 'ma6_prefix')])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-3000:])
    sys.exit(1)
PYEOF
[ -n "$BUILD_ONLY" ] && exit 0
"$WORK/ma6_prefix" "$STEMS" | tee "$WORK/ma6_prefix_output.txt"
