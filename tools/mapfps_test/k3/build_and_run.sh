#!/bin/bash
# K3: build and run the kr / mb pins (k3_pins.h) on their own.
#
# usage: tools/mapfps_test/k3/build_and_run.sh
#   BUILD_DIR   the configured build tree to lift flags and the SharedCode lib
#               from (default: build), as tools/mapfps_test/build_and_run.sh
#               step 2 does.
#   K3_OUT      where the binary goes (default: a temporary directory).
set -e
cd "$(dirname "$0")/../../.."
BUILD_DIR="${BUILD_DIR:-build}"
OUT="${K3_OUT:-$(mktemp -d)/k3_pins}"

python3 - "$BUILD_DIR" "$OUT" <<'PYEOF'
import json, sys, shlex, subprocess, os
bd, out_bin = sys.argv[1], sys.argv[2]
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
        'tools/mapfps_test/k3/k3_pins_main.cpp',
        os.path.join(bd, 'EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a'),
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', out_bin])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-6000:])
    sys.exit(1)
PYEOF
"$OUT"
