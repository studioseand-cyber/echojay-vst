#!/bin/bash
# Build + run slotwet_readback: the COMMIT 3 (17 Sep 2026) transport gate — a
# ctrl-cmd slotWet verb must reach the Link's ChainHost slot wet (the object
# that processes audio). Links the REAL EchoJayLink SharedCode lib; compile
# flags lifted from the LINK target's LinkProcessor.cpp entry (linksync_test's
# shape). LIB=<path> selects the library under test (before/after).
# Isolation: private ECHOJAY_STATE_HOME + HOME + EJ_STATE_TEST_HOME.
: "${ECHOJAY_STATE_HOME:=$(mktemp -d /tmp/echojay-slotwet-state.XXXXXX)}"; export ECHOJAY_STATE_HOME
ISOHOME="$(mktemp -d /tmp/echojay-slotwet-home.XXXXXX)"
export HOME="$ISOHOME"; export EJ_STATE_TEST_HOME="$ISOHOME"
echo "isolated state root: $ECHOJAY_STATE_HOME"
echo "isolated HOME:       $ISOHOME"

set -e
cd "$(dirname "$0")/../.."
REPO="$(pwd)"
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
LIB="${LIB:-build-release/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a}" \
python3 - "$SCRATCH" "$REPO" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch, repo = sys.argv[1], sys.argv[2]
cc = json.load(open(os.path.join(repo,'build/compile_commands.json')))
entry = [e for e in cc if e['file'].endswith('Source/LinkProcessor.cpp')
                        and 'CMakeFiles/EchoJayLink.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('LinkProcessor.cpp') or a.endswith('.o'): continue
    out.append(a)
lib = os.environ['LIB']
print("LIB under test:", lib)
cmd = (['clang++'] + out + ['-I', os.path.join(repo,'Source'),
        os.path.join(repo,'tools/slotwet_readback/slotwet_readback.cpp'), lib,
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', f'{scratch}/slotwet_readback'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-6000:]); sys.exit(1)
print("BUILT")
PYEOF
"$SCRATCH/slotwet_readback"
