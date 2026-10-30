#!/bin/bash
# Build + run the fpForIdentity join self-test against the real header.
# Usage: ./build_and_run.sh   (from anywhere; requires build/ to exist)
#
# Same shape as tools/paramapply_test: lift the real compile flags out of
# build/compile_commands.json so the test TU sees exactly the JUCE module
# defines and include paths the plugin is built with, then link the Release
# SharedCode static lib for the JUCE symbols.
#
# EchoJayParamMaps.h is header-only inline code, so the test TU compiles the
# SHIPPED implementation directly - there is no lib copy of these functions
# to drift from. The test also reads Source/ChainHost.cpp from the repo root
# (structural wiring check), so it must run with cwd at the repo root, which
# the cd below guarantees.
# ISOLATION (6 Sep 2026 ruling): this harness runs against a PRIVATE state root,
# never the user's live registry / auth.json / caches. Set ECHOJAY_STATE_HOME to
# reuse a root; unset, a fresh temporary one is created and named.
: "${ECHOJAY_STATE_HOME:=$(mktemp -d /tmp/echojay-harness-state.XXXXXX)}"; export ECHOJAY_STATE_HOME
echo "isolated state root: $ECHOJAY_STATE_HOME"

set -e
cd "$(dirname "$0")/../.."

# ---- THE LINK COMPILE GATE (5 Sep 2026) ----------------------------------
# WHY THIS EXISTS. On 5 September a required LoadOrigin argument was added to
# ChainHost::loadPluginAsync. The main SharedCode archive built clean and this
# gate went green at 654 ok through six mutations, while LinkProcessor.cpp did
# not compile at all: a lambda driving the Link's per-slot loads had not
# captured the new parameter. The defect surfaced only at the FORMAT build, on
# EchoJayLink_AU, which is the last step before installing.
#
# The reason the gate could not see it is structural, not an oversight. The
# archive linked below is the MAIN plugin's translation units; LinkProcessor.cpp
# and LinkEditor.cpp are in a separate juce_add_plugin target (CMakeLists.txt
# :454) with its own SharedCode archive, and nothing in this harness compiled
# them. Any change to a ChainHost signature the Link calls had the same blind
# spot.
#
# COMPILE ONLY, DELIBERATELY. The two archives are NOT linked together: both
# plugins define JUCE plugin-entry symbols and a createPluginFilter, so a shared
# link is a duplicate-symbol problem to solve for no extra coverage. Building
# the Link's archive proves its translation units still compile against the
# headers this commit changed, which is the whole of what was missing. No pins
# are added here and none are wanted: the compile IS the assertion.
#
# IT STOPS THE GATE. This is a script, not something pasted into a shell, so a
# hard exit is correct: a warning that scrolled past would leave the same hole
# it is closing. set -e is already on, and the explicit block is here so the
# failure says what it was rather than trailing a wall of compiler output.
#
# It costs nothing when the Link is untouched: make rebuilds only what changed,
# so an unrelated commit sees "Built target EchoJayLink" and moves on. It does
# NOT build the AU or VST3 bundles, so an installed plugin is never disturbed
# by running the gate.
echo "== gate step 1/2: the Link's SharedCode archive must compile =="
# -j 4, NOT THE CORE COUNT (18 Sep 2026). This read -j"$(sysctl -n hw.ncpu)",
# which is 10 on the 16 GiB machine this repo is built on, and it runs inside
# the pre-commit gate. Measured one compile at a time with /usr/bin/time -l:
# PluginEditor.cpp peaks at 2,535,456,768 bytes (2.36 GiB) against 0.35 GiB for
# a unit without the generated headers, and FIVE units share its 44 MB
# filmstrip header (the other four were not measured one by one). Ten jobs can
# hold several of those at once beside everything else the machine runs, which
# wants more memory than it has. At -j 4 the main archive peaked at 5.70 GiB
# and this Link archive, built from nothing, at 4.90 GiB in 244 s, with no swap;
# ten jobs is an extrapolation from those, not a measurement.
# The machine has already been taken down once by an unpinned job count: on
# 17 Sep a clean rebuild ran with a bare -j, which GNU make treats as unlimited.
# A bare -j and the core count are both "however many", and neither belongs here.
if ! ( cd build && make -j 4 EchoJayLink ); then
    echo "" >&2
    echo "GATE FAILED: EchoJayLink did not compile." >&2
    echo "The Link's translation units (LinkProcessor.cpp, LinkEditor.cpp) are" >&2
    echo "outside the main archive this harness links, so this step is the only" >&2
    echo "thing that compiles them. Fix the Link before the pins are meaningful." >&2
    exit 1
fi
echo "== gate step 2/2: mapfps_test =="

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
python3 - "$SCRATCH" <<'PYEOF'
import json, sys, shlex, subprocess, os, glob, time
scratch = sys.argv[1]
# ---- WHICH TREE'S ARCHIVE (28 Oct 2026) -----------------------------------------------------
# The path below was hardcoded to build/, and tonight that archive is routinely DELETED: a header
# change means removing the archives in build, build-guards and build-release, and only the tree
# being gated is rebuilt. So this harness failed to COMPILE and reported exit 1, which reads as a
# red row rather than as "nothing to link against".
#
# EJ_MAPFPS_LIB names the archive to link; the default is the old path, so an existing caller is
# unaffected. build-quick is the usual answer (arm64, no LTO - the same tree the fast guards use).
libPath = os.environ.get('EJ_MAPFPS_LIB',
                         'build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a')
if not os.path.exists(libPath):
    sys.exit("mapfps_test: no archive to link at '%s'. Build one (cmake --build build-quick "
             "--target EchoJay) or set EJ_MAPFPS_LIB to a tree that has one." % libPath)
# ---- AND IT MUST NOT BE OLDER THAN THE SOURCE (30 Oct 2026) --------------------------------
#
# This cost a commit and fourteen minutes of a machine. The hang fix (an unprepared MeterEngine
# spinning in its hop loop) went into Source at 08:16; build/'s archive was from 07:32; the
# pre-commit gate said "archive-present" and linked it. So me PIN1 - the leg written FOR that very
# fault - called the unprepared path in PRE-FIX code, span, grew, and the OS killed the process
# with SIGKILL. The suite printed "GATE FAIL: mapfps_test" with no failing leg, which is the least
# useful thing it could have said.
#
# A behavioural leg is a statement about the BINARY, so linking an archive older than the source is
# not a near miss, it is testing a different program. compile_link_harness.sh has refused this for
# the Link side for weeks; this is the same rule for the main archive.
_newer = [f for f in glob.glob('Source/*.cpp') + glob.glob('Source/*.h') + glob.glob('Source/*.mm')
          if os.path.getmtime(f) > os.path.getmtime(libPath)]
if _newer:
    _when = time.strftime('%d %b %H:%M', time.localtime(os.path.getmtime(libPath)))
    sys.exit("mapfps_test: REFUSED - the archive is OLDER than the source it would be linked\n"
             "  against, so every behavioural leg would be about a different program.\n"
             "  archive: %s  (%s)\n"
             "  newer:   %s%s\n"
             "  rebuild it:  cmake --build <that tree> -j 4 --target EchoJay"
             % (libPath, _when, ', '.join(sorted(os.path.basename(f) for f in _newer)[:6]),
                '' if len(_newer) <= 6 else ' (+%d more)' % (len(_newer) - 6)))
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('PluginEditor.cpp')][0]
args = shlex.split(entry['command'])
# ---- AND THE ARCHES MUST MATCH THE ARCHIVE (28 Oct 2026) ------------------------------------
# The flags are lifted from a UNIVERSAL build, so they carry -arch x86_64 as well as -arch arm64.
# Link those against an arm64-only archive (build-quick's, the fast-guard tree) and the only thing
# that comes back is "symbol(s) not found for architecture x86_64" - which reads like a missing
# symbol and is really a missing slice. Ask the archive what it holds and keep only those arches.
# tools/hosted_exit_probe/build_and_run.sh does the same thing for the same reason.
try:
    have = subprocess.run(['lipo', '-archs', libPath], capture_output=True, text=True,
                          check=True).stdout.split()
except Exception:
    have = []
out, skip, dropped = [], False, []
for a in args[1:]:
    if skip:
        if skip == 'arch':
            if have and a not in have: dropped.append(a)
            else: out += ['-arch', a]
            skip = False; continue
        skip = False; continue
    if a == '-arch': skip = 'arch'; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('PluginEditor.cpp') or a.endswith('.o'): continue
    out.append(a)
if dropped:
    print("mapfps_test: the archive holds %s, so dropping -arch %s"
          % ('/'.join(have), ' and -arch '.join(dropped)))
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'),
        'tools/mapfps_test/mapfps_test.cpp',
        libPath,
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-lcurl',
        '-o', f'{scratch}/mapfps_test'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-3000:])
    sys.exit(1)
PYEOF
"$SCRATCH/mapfps_test"
