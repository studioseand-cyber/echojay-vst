#!/bin/bash
# Build + run the overlay_gate harness against the SHIPPING SharedCode object code.
#   bash build_and_run.sh [source.cpp]      (default: borrowkept_harness.cpp)
#   LIB=<path/to/libEchoJay V2_SharedCode.a> selects the library under test
#   (RED against the pre-fix lib, GREEN against the rebuilt one).
# ISOLATION: private ECHOJAY_STATE_HOME + HOME + EJ_STATE_TEST_HOME so nothing
# here can touch ~/Library/EchoJay, ~/Library/Application Support/EchoJay or
# ~/Documents/EchoJay. Same compile recipe as tools/borrowhost_test.
: "${ECHOJAY_STATE_HOME:=$(mktemp -d /tmp/echojay-overlaygate-state.XXXXXX)}"; export ECHOJAY_STATE_HOME
ISOHOME="$(mktemp -d /tmp/echojay-overlaygate-home.XXXXXX)"
export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
echo "isolated state root: $ECHOJAY_STATE_HOME"
echo "isolated HOME:       $ISOHOME"

set -e
cd "$(dirname "$0")/../.."
REPO="$(pwd)"
SRC="${1:-tools/overlay_gate_harness/overlay_gate_harness.cpp}"
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
LIB="${LIB:-build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a}" \
python3 - "$SCRATCH" "$REPO" "$SRC" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch, repo, src = sys.argv[1], sys.argv[2], sys.argv[3]
cc = json.load(open(os.path.join(repo,'build/compile_commands.json')))
entry = [e for e in cc if e['file'].endswith('Source/ChainHost.cpp')
                        and 'CMakeFiles/EchoJay.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('ChainHost.cpp') or a.endswith('.o'): continue
    out.append(a)
lib = os.environ['LIB']
cmd = (['clang++'] + out + ['-I', os.path.join(repo,'Source'),
        os.path.join(repo, src), lib,
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', f'{scratch}/bin'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print("BUILD FAILED:"); print(r.stderr[-5000:]); sys.exit(1)
print("BUILT")
PYEOF
"$SCRATCH/bin"
