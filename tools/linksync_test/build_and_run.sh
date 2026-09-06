#!/bin/bash
# Build + run linksync_test: the FUNCTIONAL display-parity gate — a structure
# plan applied through LinkProcessor::applyStructurePlanAndSync must move the
# EDITOR-FACING chain model, not just chainHost. Links the REAL EchoJayLink
# SharedCode lib so the object code under test is what ships in the Link.
#
# Same shape as tools/borrowhost_test/build_and_run.sh, but the compile flags
# are lifted from the LINK target's LinkProcessor.cpp entry so the JUCE module
# defines match the Link build exactly.
#
# HOME IS SANDBOXED for the run; the binary refuses to start unless JUCE's
# app-data directory resolves under EJ_STATE_TEST_HOME.
# ISOLATION (6 Sep 2026 ruling): this harness runs against a PRIVATE state root,
# never the user's live registry / auth.json / caches. Set ECHOJAY_STATE_HOME to
# reuse a root; unset, a fresh temporary one is created and named.
: "${ECHOJAY_STATE_HOME:=$(mktemp -d /tmp/echojay-harness-state.XXXXXX)}"; export ECHOJAY_STATE_HOME
echo "isolated state root: $ECHOJAY_STATE_HOME"

set -e
cd "$(dirname "$0")/../.."
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
python3 - "$SCRATCH" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch = sys.argv[1]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('Source/LinkProcessor.cpp')
                        and 'CMakeFiles/EchoJayLink.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('LinkProcessor.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'),
        'tools/linksync_test/linksync_test.cpp',
        'build/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/linksync_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-4000:])
    sys.exit(1)
PYEOF
mkdir -p "$SCRATCH/home"
"$SCRATCH/linksync_test"
