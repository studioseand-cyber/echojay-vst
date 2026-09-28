#!/bin/bash
# Build the AU instantiation probe (mirrors tools/settings_snapshot/build_and_run.sh:
# reuse the SHIPPING compile flags from compile_commands.json, link the shipping
# SharedCode lib so the HEADLESS AU format module + registration match the app).
# REAL home on purpose: iLok/Softube licensing must resolve exactly as in-host.
# Prints the built binary path; the caller runs it (with its own timeout/kill).
set -e
cd "$(dirname "$0")/../.."
OUT="${OUT:-/tmp/au_instantiate_probe}"
python3 - "$OUT" <<'PYEOF'
import json, sys, shlex, subprocess, os
out_bin = sys.argv[1]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('Source/ChainHost.cpp') and 'CMakeFiles/EchoJay.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('ChainHost.cpp') or a.endswith('.o'): continue
    out.append(a)
lib = os.environ.get('LIB', 'build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a')
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'),
        'tools/au_instantiate_probe/au_instantiate_probe.cpp', lib,
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', out_bin])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-4000:]); sys.exit(1)
print("BUILT:", out_bin)
PYEOF
