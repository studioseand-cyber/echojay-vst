#!/bin/bash
# compile ONE source against the LINK's shipping archive (build-release), flags lifted from the Link target's LinkProcessor.cpp entry
# ISOLATION (6 Sep 2026 ruling): this harness runs against a PRIVATE state root,
# never the user's live registry / auth.json / caches. Set ECHOJAY_STATE_HOME to
# reuse a root; unset, a fresh temporary one is created and named.
: "${ECHOJAY_STATE_HOME:=$(mktemp -d /tmp/echojay-harness-state.XXXXXX)}"; export ECHOJAY_STATE_HOME
echo "isolated state root: $ECHOJAY_STATE_HOME"

set -e; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
SRC="$1"; OUT="$2"

# 21t-m (29 Sep 2026): THE ARCHIVE AND THE HEADERS MUST BE THE SAME TREE, AND THIS IS WHERE THAT IS CHECKED.
# What happened: item 5 gave ChainHost a new member, the fast gate rebuilt build-guards (the V2 archive) but
# NOTHING rebuilds the LINK archive, and every two-process guard then compiled the NEW headers against a Link
# archive four hours older. The layouts disagreed, and lease_id_guard and level_match_guard both died with
# SIGSEGV on the link side before printing a single assertion - which reads exactly like a product crash.
# A mismatch is now a REFUSAL with the reason, not a segfault. EJ_LINK_LIB/EJ_SRC_ROOT deliberately skip this:
# pairing a pre-round archive with pre-round headers is how a RED run is taken, and that pairing is consistent.
if [ -z "${EJ_LINK_LIB:-}" ] && [ -z "${EJ_SRC_ROOT:-}" ]; then
    LIB="build-release/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a"
    if [ ! -f "$LIB" ]; then
        echo "  FAIL  the Link archive does not exist at $LIB - build it first:"
        echo "        cmake --build build-release -j 4 --target EchoJayLink"
        exit 1
    fi
    NEWER=$(find Source -newer "$LIB" -name '*.h' -o -newer "$LIB" -name '*.cpp' 2>/dev/null | head -5)
    if [ -n "$NEWER" ]; then
        echo "  FAIL  the Link archive is OLDER than the headers this would compile against, so the two would"
        echo "        disagree about every object's layout. That is a segfault on the link side, not a result."
        echo "        archive: $(stat -f '%Sm' "$LIB")"
        echo "        newer sources:"; echo "$NEWER" | sed 's/^/          /'
        echo "        rebuild it:  cmake --build build-release -j 4 --target EchoJayLink"
        exit 1
    fi
fi
python3 - "$SRC" "$OUT" <<'PYEOF'
import json, sys, shlex, subprocess, os
src, out_bin = sys.argv[1], sys.argv[2]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('Source/LinkProcessor.cpp') and 'CMakeFiles/EchoJayLink.dir' in e['command']][0]
args = shlex.split(entry['command']); out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('LinkProcessor.cpp') or a.endswith('.o'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath(os.path.join(os.environ.get('EJ_SRC_ROOT', '.'), 'Source')), src,
        os.environ.get('EJ_LINK_LIB', 'build-release/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a'),   # EJ_LINK_LIB / EJ_SRC_ROOT: a RED run pairs the pre-round Link archive with the pre-round headers
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI','-framework','AudioToolbox','-framework','Accelerate',
        '-framework','QuartzCore','-framework','IOKit','-framework','Security','-framework','WebKit','-framework','Metal','-framework','MetalKit',
        '-framework','CoreAudioKit','-framework','UniformTypeIdentifiers','-framework','AVFoundation','-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', out_bin])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode: print(r.stderr[-3000:]); sys.exit(1)
print("compiled", out_bin)
PYEOF
