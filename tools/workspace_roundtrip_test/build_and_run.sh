#!/bin/bash
# Build + run the workspace round-trip self-test against the real objects.
# Usage: ./build_and_run.sh   (from this directory; requires build/ to exist)
#
# EJ_SELFTEST compiles the test body into a FRESH EchoJayWorkspace.o (the
# release static lib carries only the stub); the rest links from the lib.
#
# ODR GUARD RAIL: this mixed build (one TU with EJ_SELFTEST, the lib
# without) is safe ONLY while EJ_SELFTEST gates a function BODY in a .cpp.
# It must NEVER appear in a header and must never gate anything that
# changes struct layout, class size, or vtable shape — such a mismatch
# fails as random runtime memory corruption, not a link error.
# ISOLATION (6 Sep 2026 ruling): this harness runs against a PRIVATE state root,
# never the user's live registry / auth.json / caches. Set ECHOJAY_STATE_HOME to
# reuse a root; unset, a fresh temporary one is created and named.
: "${ECHOJAY_STATE_HOME:=$(mktemp -d /tmp/echojay-harness-state.XXXXXX)}"; export ECHOJAY_STATE_HOME
echo "isolated state root: $ECHOJAY_STATE_HOME"

set -e
cd "$(dirname "$0")/../.."
# ---- WHAT THIS NEEDS, NAMED BEFORE IT IS MISSED (28 Oct 2026) -----------------------------
# This test compiles EchoJayWorkspace.cpp a second time with EJ_SELFTEST, using the PRODUCT'S OWN
# compile line lifted out of build/compile_commands.json - that is how it is guaranteed to be
# compiled with the flags the plugin is. Without the file the python below threw and the gate said
# only "could not be built", which sent K looking for a broken test instead of a missing flag.
if [ ! -f build/compile_commands.json ]; then
  echo "REFUSING TO RUN - build/compile_commands.json does not exist."
  echo "  It is what this test lifts the product's own compile line from."
  echo "  Create it:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
  echo "  (an existing build/ only needs the re-configure; nothing is rebuilt by it)"
  exit 2
fi
if [ ! -f "build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a" ]; then
  echo "REFUSING TO RUN - build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a does not exist."
  echo "  This test links the V2 archive from the build/ tree."
  echo "  Build it:  cmake --build build -j 4 --target EchoJay"
  echo "  (a header change deletes the archives in build, build-guards AND build-release, and this"
  echo "   is the one that then has to be rebuilt before this test can run at all)"
  exit 2
fi
SCRATCH=$(mktemp -d)
python3 - "$SCRATCH" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch = sys.argv[1]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('EchoJayWorkspace.cpp')][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('EchoJayWorkspace.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-DEJ_SELFTEST=1', '-I', os.path.abspath('Source'),
        'Source/EchoJayWorkspace.cpp', 'tools/workspace_roundtrip_test/rt_test.cpp',
        'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/rt_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode: print(r.stderr[-2000:]); sys.exit(1)
PYEOF
"$SCRATCH/rt_test"
