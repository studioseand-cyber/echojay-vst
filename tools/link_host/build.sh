#!/bin/bash
# =============================================================================
#  Build tools/link_host (Sean, 16 Oct 2026): ONE Link, in its own process.
# =============================================================================
#  The same compile idiom as the rig: take the flags cmake used for ChainHost.cpp
#  out of compile_commands.json and link the shipping SharedCode archive, so this
#  host is built against exactly the code that ships. One argument: where to put
#  the binary (default: alongside the rig's scratch build).
# =============================================================================
set -u
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
OUT="${1:-$ROOT/build/link_host}"

python3 - "$OUT" "$ROOT/tools/link_host/link_host.mm" <<'PYEOF'
import json, sys, shlex, subprocess, os
out_path, src = sys.argv[1], sys.argv[2]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('Source/ChainHost.cpp') and 'CMakeFiles/EchoJay.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('ChainHost.cpp') or a.endswith('.o'): continue
    if a.startswith('-flto'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'), src,
        os.environ.get('LIB', 'build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a'),
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', out_path])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-6000:]); sys.exit(1)
print("link_host ->", out_path)
PYEOF
