#!/bin/bash
# Merge-gate harnesses (6 Sep 2026): compile ONE source against the SHIPPING
# SharedCode lib (build-release, the object code that is installed), same
# shape as tools/settings_snapshot/build_and_run.sh. LIB=<path> overrides.
# Usage: build_and_run.sh <source.cpp|.mm> [args...]
set -e
cd "$(dirname "$0")/../.."
SRC="$1"; shift
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
python3 - "$SCRATCH" "$SRC" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch, src = sys.argv[1], sys.argv[2]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('Source/ChainHost.cpp') and 'CMakeFiles/EchoJay.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('ChainHost.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'), src,
        os.environ.get('LIB', 'build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a'),
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', f'{scratch}/harness'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-4000:]); sys.exit(1)
PYEOF
"$SCRATCH/harness" "$@"
