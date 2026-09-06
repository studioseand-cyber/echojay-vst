#!/bin/bash
# Build + run the ring seek/stamp self-test (stage 0 of remote editing).
# Usage: ./build_and_run.sh   (from anywhere; requires build/ to exist)
#
# Same shape as tools/linkmixer_test/build_and_run.sh: lift the real compile
# flags out of build/compile_commands.json so the test TU sees exactly the
# JUCE module defines and include paths the plugins are built with, then link
# the Release SharedCode static lib for the JUCE symbols.
#
# The ring functions under test are header-inline in LinkShm.h, so this TU's
# instantiation IS the shipping arithmetic; the SharedCode link supplies the
# JUCE runtime the header depends on rather than a second copy of the ring.
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
entry = [e for e in cc if e['file'].endswith('PluginEditor.cpp')][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('PluginEditor.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'),
        'tools/ringseek_test/ringseek_test.cpp',
        'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/ringseek_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-3000:])
    sys.exit(1)
PYEOF
"$SCRATCH/ringseek_test"
