#!/bin/bash
# Build + run mapfps_test under AddressSanitizer and UndefinedBehaviorSanitizer.
#
# WHY THIS EXISTS. Two consecutive commits to EJReferenceIndex.h produced a
# memory-class defect the compiler could not see: a Ptr taking ownership of a
# std::unique_ptr's pointee (use-after-free, segfault on first run), and a
# reinterpret_cast on a null pointer (undefined behaviour, compiled clean). Both
# were found by a crash and by an eye. Neither route is reliable.
#
# IT IS NOT PART OF THE ORDINARY GATE, deliberately: an instrumented build of
# this translation unit is slow and the gate is already the slowest part of a
# commit. Run it by hand when a commit touches a header that allocates, parses
# or takes a lock, and before a release build.
#
# EJ_ASAN_NO_NETWORK is defined HERE and nowhere else. The ordinary gate still
# runs the network-touching pins. See open list 140: this does not solve the
# gate reaching production.
#
# IT ALSO DOES NOT GET THE INSTRUMENTED RUN TO THE ri PINS. Measured 12 Sep:
# with the network excluded this harness still dies deterministically at check
# ~192 in the 6c param-read pins, which construct real processors, ~800 checks
# before the reference-index pins. Raising the main thread stack 8 MB to 64 MB
# changed nothing. That blocker is unresolved.
#
# WHAT ANSWERS THE QUESTION INSTEAD is asan_refindex.cpp beside this file: a
# standalone harness that drives EJReferenceIndex.h alone and completes.
# Run that when the question is "does this header have a memory defect".
#
# ASAN_OPTIONS NOTE: detect_leaks is NOT SUPPORTED on macOS. Setting it makes
# ASan exit 1 before running a single line, which reads exactly like a clean
# run with zero findings. It cost two runs on 12 Sep. See open list 141.
set -e
cd "$(dirname "$0")/../.."

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
python3 - "$SCRATCH" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch = sys.argv[1]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('PluginEditor.cpp')][0]
args = shlex.split(entry['command'])[1:]
out, skip = [], 0
for t in args:
    if skip: skip -= 1; continue
    if t == '-o': skip = 1; continue
    if t == '-c': continue
    if t == '-arch': skip = 1; continue          # single arch: instrumented fat is slow
    if t == '-flto': continue                    # LTO + sanitizer is slow and noisy
    if t.endswith('PluginEditor.cpp') or t.endswith('.o'): continue
    out.append(t)
cmd = (['clang++'] + out +
       ['-arch', 'arm64',
        '-DEJ_ASAN_NO_NETWORK=1',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-O1',
        '-I', os.path.abspath('Source'),
        'tools/mapfps_test/mapfps_test.cpp',
        'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a',
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/mapfps_asan'])
r = subprocess.run(cmd, capture_output=True, text=True)
print("ASAN BUILD rc:", r.returncode)
if r.returncode:
    print(r.stderr[-5000:]); sys.exit(1)

env = dict(os.environ,
           ASAN_OPTIONS="detect_leaks=0:abort_on_error=0",
           UBSAN_OPTIONS="print_stacktrace=1")
p = subprocess.run([f'{scratch}/mapfps_asan'], capture_output=True, text=True, env=env)

# POSITIVE CONTROL (open list 141): prove the manipulation applied. A run that
# still reached the network did NOT exclude it, and is not a result.
reached = (p.stdout + p.stderr).count('EJNet: config fetch')
print("POSITIVE CONTROL, network calls observed:", reached, "(must be 0)")
print("ASAN RUN rc:", p.returncode)
summary = [l for l in p.stdout.split('\n') if l.startswith(('PASS','FAIL'))]
print("summary:", summary[-1] if summary else "(none: the run did not finish)")
print("ri PIN lines:", sum(1 for l in p.stdout.split('\n') if 'ri PIN' in l))
findings = [l for l in (p.stdout + p.stderr).split('\n')
            if 'ERROR: AddressSanitizer' in l or 'runtime error' in l or 'SUMMARY: ' in l]
print("SANITIZER FINDINGS:", len(findings))
for l in findings[:40]: print("   ", l)
PYEOF
