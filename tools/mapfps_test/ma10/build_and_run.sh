#!/bin/bash
# MA10 (C4 section 10): build the offline crest harness and run it on the stems.
#
# usage: tools/mapfps_test/ma10/build_and_run.sh "<stem folder>"
#   BUILD_DIR   the configured build tree to lift flags and the SharedCode lib
#               from (default: build). Its compile_commands.json and
#               libEchoJay V2_SharedCode.a must exist.
#
# The same shape as tools/mapfps_test/build_and_run.sh step 2: the real compile
# flags, so juce::dsp::FFT here is the FFT MeterEngine runs, linked against the
# SharedCode archive for the JUCE symbols. Every generated file goes in
# tools/mapfps_test/ma10/work/. The stems are only ever opened for reading.
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
        'tools/mapfps_test/ma10/ma10.cpp',
        os.path.join(bd, 'EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a'),
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', os.path.join(work, 'ma10')])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-3000:])
    sys.exit(1)
PYEOF
"$WORK/ma10" "$STEMS" "$WORK" | tee "$WORK/ma10_output.txt"
