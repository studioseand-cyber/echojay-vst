#!/bin/bash
# bandpeak_cadence_guard, built the way every guard that needs the product's own DSP is: compiled
# against the V2 archive by PATH, never against the juce:: module targets (the guard-suite rule).
#
# EJ_BP_LIB names the archive; the default is this worktree's build-guards. This is a SIDE BRANCH
# (a/bandpeak-256) and a worktree, so it builds and self-tests here and installs nothing.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
LIB="${EJ_BP_LIB:-$ROOT/build-guards/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a}"
if [ ! -f "$LIB" ]; then
  echo "REFUSING TO RUN - the V2 archive does not exist at $LIB"
  echo "  cmake --build build-guards -j 4 --target EchoJay   (in this worktree)"
  exit 2
fi
OUT="${EJ_BP_OUT:-$(mktemp -d /tmp/echojay-bp256.XXXXXX)}"
# The compile line comes from the tree's own compile_commands.json, like link_host's build does, so
# the guard is compiled with exactly the flags the product is.
python3 - "$LIB" "$OUT" <<'PY'
import json, os, shlex, subprocess, sys
lib, out = sys.argv[1], sys.argv[2]
root = os.getcwd()
cc = None
for tree in ('build-guards', 'build', 'build-release'):
    p = os.path.join(root, tree, 'compile_commands.json')
    if os.path.exists(p): cc = p; break
if not cc: print("no compile_commands.json in build-guards/build/build-release"); sys.exit(2)
db = json.load(open(cc))
entry = next((e for e in db if e['file'].endswith('MeterEngine.cpp')), None) or db[0]
args = shlex.split(entry['command'])
keep, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('.cpp') or a.endswith('.mm') or a.endswith('.o'): continue
    if a.startswith('-flto'): continue
    keep.append(a)
# arm64 only if the archive is: a universal link of a thin archive fails on the missing slice.
archs = subprocess.run(['lipo','-archs',lib], capture_output=True, text=True).stdout.split()
out_args = []
i = 0
while i < len(keep):
    if keep[i] == '-arch':
        if keep[i+1] in archs: out_args += ['-arch', keep[i+1]]
        i += 2; continue
    out_args.append(keep[i]); i += 1
exe = os.path.join(out, 'bandpeak_cadence_guard')
cmd = (['clang++'] + out_args + ['-I', os.path.join(root, 'Source'),
        os.path.join(root, 'tools/bandpeak_cadence_guard/guard.cpp'), lib,
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', exe])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-6000:]); sys.exit(1)
print(exe)
PY
RC=$?
[ $RC -ne 0 ] && { echo "the guard did not build"; exit $RC; }
EXE="$OUT/bandpeak_cadence_guard"
[ -x "$EXE" ] || { echo "no binary at $EXE"; exit 2; }
"$EXE"
