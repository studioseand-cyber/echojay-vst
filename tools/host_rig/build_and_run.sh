#!/bin/bash
# =============================================================================
#  THE HOST RIG (Sean's ask A, 13 Oct 2026) - build it, start the mock, run it
# =============================================================================
#  Compiles tools/host_rig/host_rig.mm against the shipping SharedCode lib (the
#  same idiom as tools/merge_gate_tests/build_and_run.sh), starts the local mock
#  server on a free port, and runs the rig against the BUILT bundles.
#
#  ISOLATION, twice over: a private ECHOJAY_STATE_HOME (6 Sep ruling) and the
#  hook's own EJ_RIG_HOOK=1. The rig refuses to start without both, so it cannot
#  drive the UI against Sean's live state.
#
#  EXIT CODES
#    0   green
#    1   red - a leg failed
#    2   refused - a precondition was missing (no isolated root, no bundles)
#    77  SKIPPED LOUDLY - no GUI session, so no editor can be opened. Sean's
#        instruction: S6 must skip loudly, never pass quietly.
#
#  Usage: build_and_run.sh [V2.component] [Link.component]
# =============================================================================
: "${ECHOJAY_STATE_HOME:=$(mktemp -d /tmp/echojay-rig-state.XXXXXX)}"; export ECHOJAY_STATE_HOME
export EJ_RIG_HOOK=1
echo "isolated state root: $ECHOJAY_STATE_HOME"

set -u
cd "$(dirname "$0")/../.."
ROOT=$(pwd)

# ---- LIVE MODE (B's local server, 14 Oct 2026) ------------------------------
# EJ_RIG_LIVE=<port> runs against B's local server instead of the mock: the real routes, the real model, the
# real maps. The rig writes the token /api/forge-dev-login hands out into auth.json, which is the account
# road B named. Everything else is identical - the same legs, the same assertions on real state.
LIVE_PORT="${EJ_RIG_LIVE:-}"

V2_COMP="${1:-$ROOT/build-guards/EchoJay_artefacts/Release/AU/EchoJay V2.component}"
LINK_COMP="${2:-$ROOT/build-guards/EchoJayLink_artefacts/Release/AU/EchoJay Link.component}"

if [ ! -d "$V2_COMP" ] || [ ! -d "$LINK_COMP" ]; then
  echo "REFUSED: the built AU bundles are not where I looked:"
  echo "  $V2_COMP"
  echo "  $LINK_COMP"
  echo "Build them first (cmake --build build-guards -j 4 --target EchoJay EchoJayLink)."
  exit 2
fi

SCRATCH=$(mktemp -d)
MOCK_PID=""
cleanup() {
  [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
  rm -rf "$SCRATCH"
}
trap cleanup EXIT

# ---- the server: B's, or the mock -------------------------------------------
if [ -n "$LIVE_PORT" ]; then
  PORT="$LIVE_PORT"
  echo "LIVE mode: B's local server on 127.0.0.1:$PORT"
  HEALTH=$(curl -s -m 5 "http://127.0.0.1:$PORT/local/health" || true)
  if [ -z "$HEALTH" ]; then echo "REFUSED: nothing answered /local/health on $PORT"; exit 2; fi
  echo "  health: $(echo "$HEALTH" | python3 -c 'import json,sys; h=json.load(sys.stdin); print("ok="+str(h.get("ok")), "agentOn="+str(h.get("agentOn")), "mappedControls="+str((h.get("maps") or {}).get("mappedControls")), "needs="+str(h.get("needs")))' 2>/dev/null || echo "unparsed")"
  # The token, from the road B named. A rig that fabricated one was never logged in and sent nothing at all
  # for four passes - the first thing this mode does is prove it HAS an account.
  LOGIN=$(curl -s -m 8 -X POST "http://127.0.0.1:$PORT/api/forge-dev-login" -H 'Content-Type: application/json' -d '{}' || true)
  TOKEN=$(echo "$LOGIN" | python3 -c 'import json,sys; print((json.load(sys.stdin) or {}).get("token") or "")' 2>/dev/null || true)
  if [ -z "$TOKEN" ]; then
    echo "REFUSED: /api/forge-dev-login gave no token ($LOGIN)."
    echo "  FORGE_DEV_OPEN=1 has to be in the server's environment, and --env-file is read at START, so the"
    echo "  server needs restarting with it:"
    echo "    cd ~/echojay-saas && FORGE_DEV_OPEN=1 AGENT_MODE=1 node --env-file=.env.local scripts/local-agent-server.cjs"
    exit 2
  fi
  echo "  account: $(echo "$LOGIN" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("email"), d.get("tier"))' 2>/dev/null)"
  # ---- THE RED-DIRECTION INSTRUMENT (EJ_RIG_BREAK_AGENT=1, 14 Oct 2026) ----------------------------------
  # Every agent POST gets a 401 and no turn can run, which is the 14:34 condition (a server-side 500 on the
  # agent route) reproduced on demand. It exists because that run reported SEVERAL ROWS AS PASSES while the
  # binary was never exercised: C10 passed on a rack that had not changed, because the removal it was undoing
  # had failed too. A gate against vacuous passes that cannot be driven RED is itself unfalsifiable, so this
  # switch is how the gate is proven, and it stays.
  if [ "${EJ_RIG_BREAK_AGENT:-0}" = "1" ]; then
    echo "  EJ_RIG_BREAK_AGENT=1: writing a DELIBERATELY INVALID token - every agent turn will be refused."
    echo "  Expect the capability rows to FAIL. Any row that PASSES here is a row that proves nothing."
    TOKEN="red-direction-proof.not-a-real-token"
  fi
  export EJ_RIG_TOKEN="$TOKEN"
else
  # ---- a free port, so two rigs can run side by side ------------------------
  PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
fi

# ---- the script the mock answers from --------------------------------------
# The first reply HOLDS for 6 s before its first delta: that is what makes
# "closing mid-reply" a real mid-reply rather than a race the rig hopes to win.
cat > "$SCRATCH/script.json" <<'JSON'
{
  "chat": [
    { "holdIfContains": "rig-midreply-marker", "holdMs": 6000, "deltaMs": 40,
      "text": "mock reply held back so the window could be closed mid-reply, then finished." },
    { "holdIfContains": "rig-cut-marker", "holdMs": 3000, "deltaMs": 60, "cutAfterDeltas": 3,
      "text": "this reply starts and then the socket dies before any done frame arrives" },
    { "text": "mock reply, sent quickly.", "deltaMs": 15, "holdMs": 0 }
  ],
  "agent": [
    {
      "goalContains": "rig-undo-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_u1", "name": "do", "approval": "free",
            "summary": "pre-gain 3 dB", "args": { "op": "set_pre_gain", "db": 3 } },
          { "event": "await", "ids": ["tc_u1"] } ],
        [ { "event": "round", "round": 2, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_u2", "name": "do", "approval": "free",
            "summary": "undo that", "args": { "op": "undo" } },
          { "event": "await", "ids": ["tc_u2"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 2,
            "summary": "Set the pre-gain to 3 dB, then undid it." } ]
      ]
    },
    {
      "goalContains": "rig-auto-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_a1", "name": "do", "approval": "ask_first",
            "summary": "pre-gain 4 dB", "args": { "op": "set_pre_gain", "db": 4 } },
          { "event": "tool_call", "id": "tc_a2", "name": "do", "approval": "ask_first",
            "summary": "pre-gain 6 dB", "args": { "op": "set_pre_gain", "db": 6 } },
          { "event": "await", "ids": ["tc_a1", "tc_a2"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1,
            "summary": "Set the pre-gain to 4 dB, then to 6 dB." } ]
      ]
    },
    {
      "goalContains": "rig-autoundo-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_au1", "name": "do", "approval": "ask_first",
            "summary": "undo that", "args": { "op": "undo" } },
          { "event": "await", "ids": ["tc_au1"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1,
            "summary": "Undone." } ]
      ]
    },
    {
      "goalContains": "rig-autostrange-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_as1", "name": "do", "approval": "ask_first",
            "summary": "do something this build has never heard of",
            "args": { "op": "teleport_the_rack" } },
          { "event": "await", "ids": ["tc_as1"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1, "summary": "Done." } ]
      ]
    }
  ]
}
JSON

if [ -z "$LIVE_PORT" ]; then
python3 "$ROOT/tools/host_rig/mock_server.py" --port "$PORT" --script "$SCRATCH/script.json" &
MOCK_PID=$!
fi
# Wait for it to answer rather than sleeping a guessed amount.
if [ -z "$LIVE_PORT" ]; then
  for _ in $(seq 1 50); do
    if curl -s -m 1 "http://127.0.0.1:$PORT/api/rig/requests" >/dev/null 2>&1; then break; fi
    sleep 0.1
  done
  echo "mock server on 127.0.0.1:$PORT (pid $MOCK_PID)"
fi

# ---- build the rig ----------------------------------------------------------
python3 - "$SCRATCH" "$ROOT/tools/host_rig/host_rig.mm" <<'PYEOF'
import json, sys, shlex, subprocess, os
scratch, src = sys.argv[1], sys.argv[2]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('Source/ChainHost.cpp') and 'CMakeFiles/EchoJay.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('ChainHost.cpp') or a.endswith('.o'): continue
    if a.startswith('-flto'): continue
    out.append(a)
cmd = (['clang++'] + out + ['-I', os.path.abspath('Source'), src,
        os.environ.get('LIB', 'build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a'),
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', f'{scratch}/host_rig'])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-6000:]); sys.exit(1)
PYEOF
if [ $? -ne 0 ]; then echo "REFUSED: the rig did not compile"; exit 2; fi

"$SCRATCH/host_rig" "$V2_COMP" "$LINK_COMP" "$PORT"
RC=$?
# WHAT THE SERVER ACTUALLY RECEIVED. Printed every run, because "the plugin never sent" and "the plugin sent
# and the reply was dropped" are different faults and the first run could not tell them apart.
if [ -z "$LIVE_PORT" ]; then
echo "---- requests the mock received ----"
curl -s -m 3 "http://127.0.0.1:$PORT/api/rig/requests" | python3 -c '
import json,sys
try: rows = json.load(sys.stdin).get("requests", [])
except Exception: rows = []
print(f"  {len(rows)} request(s)")
for r in rows: print("   ", r.get("path"), "chat="+str(r.get("chatId"))[:14], repr(str(r.get("message"))[:60]))
' 2>/dev/null || echo "  (the mock did not answer)"
fi
echo "host_rig exit=$RC"
exit $RC
