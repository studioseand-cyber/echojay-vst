#!/bin/bash
# Build + run the state-chunk matching / deadman self-test against the real
# ChainHost (CHAINHOST_BRIEF Part D).
# Usage: ./build_and_run.sh   (from anywhere; requires build/ to exist and the
#                              EchoJay SharedCode lib to be built)
#
# Same shape as tools/dashboard_test/build_and_run.sh: lift the real compile
# flags for ChainHost.cpp out of build/compile_commands.json so the test TU
# sees exactly the JUCE module defines and include paths the plugin is built
# with, then link the Release SharedCode static lib for the symbols. It links
# the lib rather than recompiling ChainHost.cpp so the code under test is the
# object code that ships.
#
# HOME IS SANDBOXED for the run. ChainHost persists under
# ~/Library/Application Support/EchoJay and the deadman cases write a marker
# there and blacklist a path; the binary refuses to start unless JUCE's
# app-data directory resolves under EJ_STATE_TEST_HOME, so a bare run cannot
# touch the real EchoJay folder.
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
entry = [e for e in cc if e['file'].endswith('Source/ChainHost.cpp')
                        and 'CMakeFiles/EchoJay.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('ChainHost.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'),
        'tools/state_match_test/state_match_test.cpp',
        'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/state_match_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-4000:])
    sys.exit(1)
PYEOF
mkdir -p "$SCRATCH/home"
"$SCRATCH/state_match_test"
