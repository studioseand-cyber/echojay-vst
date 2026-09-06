#!/bin/bash
# Same shape as tools/borrowhost_test/build_and_run.sh; links the SHIPPING
# SharedCode lib from build-release (the object code that is installed).
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
entry = [e for e in cc if e['file'].endswith('Source/ChainHost.cpp') and 'CMakeFiles/EchoJay.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('ChainHost.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'),
        'tools/latency_impulse_test/latency_impulse_test.cpp',
        'build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', f'{scratch}/latency_impulse_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-4000:]); sys.exit(1)
PYEOF
APP="$HOME/Library/Application Support/EchoJay"
snap() { find "$APP" -type f -print0 2>/dev/null | xargs -0 stat -f '%m %z %N' 2>/dev/null | sort; }
snap > "$SCRATCH/before.txt"
"$SCRATCH/latency_impulse_test"; RC=$?
snap > "$SCRATCH/after.txt"
if diff -q "$SCRATCH/before.txt" "$SCRATCH/after.txt" > /dev/null; then echo "app-support folder untouched ($(wc -l < "$SCRATCH/before.txt" | tr -d ' ') files, no mtime/size change)"; else echo "APP-SUPPORT FILES CHANGED:"; diff "$SCRATCH/before.txt" "$SCRATCH/after.txt" | head -10; fi
exit $RC
