#!/usr/bin/env bash
#
# which_links_are_alive.sh — answer "is that Link row a living process?"
#
# The companion question to which_build_is_installed.sh, and the one that
# cost a session an hour: the mixer strip dims on AUDIO staleness, so a
# silent live Link and a dead one paint the same picture, and the sidecar's
# revision is process-local so it proves nothing either. The ONE observable
# that distinguishes them is the registry heartbeat — bumped ~1Hz by the
# producer's message-thread timer, frozen the instant the process dies.
#
# This reads registry_v2.bin twice, 3 seconds apart, and prints a LIVE/DEAD
# verdict per registered slot. A row Logic is holding for undo (deleted
# channel, instance kept alive) reads LIVE — correctly, because it is.
#
# Usage:
#     tools/which_links_are_alive.sh
#
set -euo pipefail

python3 - <<'PYEOF'
import struct, time, os
p = os.path.expanduser(
    "~/Library/Application Support/EchoJay/link/registry_v2.bin")
if not os.path.exists(p):
    print("no registry file - no Link has ever run on this machine's dir")
    raise SystemExit(0)

# Layout (LinkShm.h): 64-byte header, then 16 slots of 128 bytes.
# Slot: inUse u32 @+0, displayName[40] @+4, heartbeat u32 @+100,
#       instanceUid[12] @+108.
def snap():
    d = open(p, "rb").read()
    out = {}
    for i in range(16):
        o = 64 + i * 128
        if not struct.unpack_from("<I", d, o)[0]:
            continue
        name = d[o+4:o+44].split(b"\x00")[0].decode(errors="replace")
        hb,  = struct.unpack_from("<I", d, o+100)
        uid  = d[o+108:o+120].split(b"\x00")[0].decode(errors="replace")
        out[i] = (name or "(untitled)", uid, hb)
    return out

a = snap()
time.sleep(3)
b = snap()
if not b:
    print("registry: no registered slots")
for i, (name, uid, hb2) in sorted(b.items()):
    hb1 = a.get(i, (None, None, None))[2]
    verdict = ("LIVE (heartbeat climbing)"
               if hb1 is not None and hb2 != hb1
               else "DEAD (heartbeat frozen)")
    print(f"slot {i:2d}: {name!r} uid={uid} hb={hb1}->{hb2}  {verdict}")
PYEOF
