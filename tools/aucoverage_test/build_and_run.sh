#!/bin/bash
# Build + run the AU coverage self-test against the real header.
# Usage: ./build_and_run.sh   (from anywhere; requires build-ejmap/ to exist)
#
# Same shape as tools/workspace_roundtrip_test: lift the real compile flags out
# of the ejextract build's compile_commands.json so the test TU sees exactly the
# JUCE module defines and include paths ejextract is built with.
#
# EchoJayAuCoverage.h is header-only inline code, so the test compiles the
# SHIPPED implementation directly -- there is no lib copy to drift from, which
# is also why these pins are honest without a rebuild. The test reads
# tools/ejextract/main.cpp from the repo root for the wiring checks, so it must
# run with cwd at the repo root, which the cd below guarantees.
set -e
cd "$(dirname "$0")/../.."
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
python3 - "$SCRATCH" <<'PYEOF'
import sys, shlex, subprocess, os, re
scratch = sys.argv[1]
# build-ejmap is a Makefile build with no compile_commands.json, so the flags
# come from the generator's own flags.make - the same defines and include paths
# ejextract itself is compiled with. Single-arch on purpose: the pins are pure
# logic, and a universal build here only doubles compile time.
fm = open('build-ejmap/CMakeFiles/ejextract.dir/flags.make').read()
def grab(key):
    m = re.search(r'^' + key + r' = (.*)$', fm, re.M)
    return shlex.split(m.group(1)) if m else []
out = grab('CXX_DEFINES') + grab('CXX_INCLUDES') + ['-std=c++17', '-O0', '-g',
       '-arch', 'arm64', '-mmacosx-version-min=11.0']
# JUCE symbols: juce_add_console_app compiles the modules INTO the target, so
# there is no static lib to link. Take the target's own module objects and drop
# main.cpp.o, which carries ejextract's main(). Same objects the shipped binary
# links, which is the point.
objs = []
for root, _, files in os.walk('build-ejmap/CMakeFiles/ejextract.dir'):
    for f in files:
        if f.endswith('.o') and f != 'main.cpp.o':
            objs.append(os.path.join(root, f))
objs.sort()
cmd = (['clang++'] + out + ['-I', os.path.abspath('tools/ejextract'),
        'tools/aucoverage_test/aucoverage_test.cpp'] + objs + [
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','CoreAudioKit',
        '-o', f'{scratch}/aucoverage_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-3000:]); sys.exit(1)
PYEOF
"$SCRATCH/aucoverage_test"
