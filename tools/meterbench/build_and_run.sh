#!/bin/bash
# Build + run the MeterEngine per-block cost benchmark against the REAL
# objects. Usage: ./build_and_run.sh   (from anywhere; requires build/)
#
# Same shape as tools/dashboard_test/build_and_run.sh: lift the real compile
# flags out of build/compile_commands.json so the test TU sees exactly the
# JUCE module defines and include paths the plugins are built with.
#
# THE DIFFERENCE FROM THE TEST HARNESSES: this one links BOTH plugins'
# SharedCode archives, one per run, because the two carry DIFFERENT builds of
# MeterEngine. EchoJay Link is compiled with ECHOJAY_NO_VISUAL_FFT=1 and
# EchoJay V2 is not, so linking each archive in turn measures the two
# shipping variants rather than a rebuild with our own flags. MeterEngine is
# pulled straight out of the archive; nothing here recompiles it.
#
# NOT A GATE: this is deliberately absent from ~/reinstall-v2.sh's self-test
# loop and must stay absent. It costs real seconds and it answers with a
# number, not a pass or a fail.
#
# Three runs per variant; take the spread across runs as the error bar. The
# committed baseline lives in meterbench.cpp's header comment.
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
# Flags from the MAIN plugin's MeterEngine TU: include paths and JUCE module
# defines. The gate itself is NOT taken from here, it comes from whichever
# archive we link.
entry = [e for e in cc if e['file'].endswith('MeterEngine.cpp')
         and 'EchoJayLink.dir' not in e['output']][0]
args = shlex.split(entry['command'])
flags, skip = [], False
for a in args[1:]:
    if skip:
        skip = False
        continue
    if a in ('-c', '-o'):
        skip = (a == '-o')
        continue
    if a.endswith('MeterEngine.cpp') or a.endswith('.o'):
        continue
    flags.append(a)

frameworks = ['-framework', 'Cocoa', '-framework', 'CoreAudio', '-framework', 'CoreMIDI',
              '-framework', 'AudioToolbox', '-framework', 'Accelerate', '-framework', 'QuartzCore',
              '-framework', 'IOKit', '-framework', 'Security', '-framework', 'WebKit',
              '-framework', 'Metal', '-framework', 'MetalKit', '-framework', 'CoreAudioKit',
              '-framework', 'UniformTypeIdentifiers', '-framework', 'AVFoundation',
              '-framework', 'CoreMedia', '-framework', 'AVKit', '-lcurl']

variants = [('ungated', 'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a'),
            ('gated',   'build/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a')]

for name, archive in variants:
    if not os.path.exists(archive):
        print('MISSING ARCHIVE: ' + archive + ' (build the plugins first)')
        sys.exit(1)
    cmd = (['clang++'] + flags + ['-I', os.path.abspath('Source'),
           'tools/meterbench/meterbench.cpp', archive]
           + frameworks + ['-o', os.path.join(scratch, 'meterbench_' + name)])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-3000:])
        sys.exit(1)
PYEOF

echo "MeterEngine per-block cost, three runs per variant"
echo "  ungated = EchoJay V2   (visual FFT compiled in)"
echo "  gated   = EchoJay Link (visual FFT compiled out)"
echo
for RUN in 1 2 3; do
  "$SCRATCH/meterbench_ungated" "ungated[$RUN]"
  "$SCRATCH/meterbench_gated"   "gated[$RUN]"
done
echo
echo "Baseline (2 Aug 2026, Apple Silicon): ungated 83.5 us/block 0.719%,"
echo "gated 73.5 us/block 0.633%. macroBands MUST read 6/6 in both."
