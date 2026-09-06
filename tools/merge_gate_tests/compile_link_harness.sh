#!/bin/bash
# compile ONE source against the LINK's shipping archive (build-release), flags lifted from the Link target's LinkProcessor.cpp entry
set -e; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
SRC="$1"; OUT="$2"
python3 - "$SRC" "$OUT" <<'PYEOF'
import json, sys, shlex, subprocess, os
src, out_bin = sys.argv[1], sys.argv[2]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('Source/LinkProcessor.cpp') and 'CMakeFiles/EchoJayLink.dir' in e['command']][0]
args = shlex.split(entry['command']); out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('LinkProcessor.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'), src,
        'build-release/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI','-framework','AudioToolbox','-framework','Accelerate',
        '-framework','QuartzCore','-framework','IOKit','-framework','Security','-framework','WebKit','-framework','Metal','-framework','MetalKit',
        '-framework','CoreAudioKit','-framework','UniformTypeIdentifiers','-framework','AVFoundation','-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', out_bin])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode: print(r.stderr[-3000:]); sys.exit(1)
print("compiled", out_bin)
PYEOF
