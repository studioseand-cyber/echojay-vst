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
# ITEM 0 IS ON BY DEFAULT FROM 16 OCT (Sean's RULE 2): the run renders as a message in the chat, and
# rows G1-G7 therefore run in S6 unasked - no EJ_AGENT_ROW, because a flag that switched rows on in
# the rig while changing nothing in the plugin is how those rows spent a week proving a configuration
# that did not ship. EJ_AGENT_PANEL=1 puts the old floating card back on both sides at once.
#
# ITEM 0b (a run surviving its window) is a SEPARATE switch and is OFF: row B3 has never shown a
# windowless run apply its change, so a close still stops the run as 15b did. EJ_AGENT_0B=1 runs
# B1-B4 against that behaviour; deliberately NOT exported here, so S6 tests the shipped build.
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
# THE SAME V2, AS A VST3 (15 Oct 2026). The lifecycle rows need a host that can really CLOSE a window, and
# the AU cannot be made to: releasing its wrapper view does not deallocate the NSView the plugin's editor
# lives in, so one EchoJayEditor survives a whole AU run. JUCE's VST3 host releases IPlugView by refcount,
# which deletes the plugin-side editor deterministically. Passed as a fourth argument; the rig treats it as
# optional, so a tree without the VST3 built still runs every other row.
V2_VST3="${4:-$ROOT/build-guards/EchoJay_artefacts/Release/VST3/EchoJay V2.vst3}"
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
    { "holdIfContains": "rig-underscore-marker", "holdMs": 0, "deltaMs": 10,
      "text": "88_Omnisphere_01 and 88_7even Kick_2_01 both sound fine to me." },
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
      "goalContains": "rig-row-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_r1", "name": "do", "approval": "ask_first",
            "summary": "pre-gain 4 dB", "args": { "op": "set_pre_gain", "db": 4 } },
          { "event": "tool_call", "id": "tc_r2", "name": "do", "approval": "ask_first",
            "summary": "pre-gain 6 dB", "args": { "op": "set_pre_gain", "db": 6 } },
          { "event": "await", "ids": ["tc_r1", "tc_r2"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1,
            "summary": "Set the pre-gain to 4 dB, then to 6 dB." } ]
      ]
    },
    {
      "goalContains": "rig-rowask-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_rq", "name": "talk", "approval": "free",
            "summary": "how firm should it be?",
            "args": { "text": "Two ways to go about this.",
                      "ask": { "question": "How firm should it be?",
                               "choices": [ { "label": "Gentle" }, { "label": "More upfront" } ],
                               "allowFreeText": false } } },
          { "event": "await", "ids": ["tc_rq"] } ],
        [ { "event": "round", "round": 2, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_rq2", "name": "do", "approval": "free",
            "summary": "pre-gain 2 dB", "args": { "op": "set_pre_gain", "db": 2 } },
          { "event": "await", "ids": ["tc_rq2"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 2,
            "summary": "Gentle it is - the pre-gain is at 2 dB." } ]
      ]
    },
    {
      "goalContains": "rig-gr-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_g1", "name": "do", "approval": "free",
            "summary": "build a compressor on this rack",
            "args": { "op": "build",
                      "chain": [ { "name": "EchoJay Compressor",
                                   "settings_structured": { "params": { "threshold_db": -24, "ratio": 4 } } } ] } },
          { "event": "await", "ids": ["tc_g1"] } ],
        [ { "event": "round", "round": 2, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_g2", "name": "check", "approval": "free",
            "summary": "gain reduction on slot 1", "args": { "what": "gr", "slot": 1 } },
          { "event": "await", "ids": ["tc_g2"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 2, "summary": "Read the gain reduction." } ]
      ]
    },
    {
      "goalContains": "rig-linkadd-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_la", "name": "do", "approval": "free",
            "summary": "add a compressor to this rack",
            "args": { "op": "add", "name": "EchoJay Compressor" } },
          { "event": "await", "ids": ["tc_la"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1,
            "summary": "Added a compressor." } ]
      ]
    },
    {
      "goalContains": "rig-eqband-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_eq", "name": "do", "approval": "free",
            "summary": "two bells on an EchoJay EQ",
            "args": { "op": "add", "name": "EchoJay EQ",
                      "settings_structured": { "params": { "band1.freq_hz": 240, "band1.gain_db": -2.5,
                                                           "band1.q": 1.2,
                                                           "band2.freq_hz": 3200, "band2.gain_db": 1.5,
                                                           "band2.q": 0.9 } } } },
          { "event": "await", "ids": ["tc_eq"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1,
            "summary": "Two bells on the EQ." } ]
      ]
    },
    {
      "goalContains": "rig-thirdparty-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_tp", "name": "do", "approval": "free",
            "summary": "add a real third-party plugin, with a setting",
            "args": { "op": "add", "name": "__TP_NAME__",
                      "settings_structured": { "controls": { "__TP_CONTROL__": "__TP_VALUE__" } } } },
          { "event": "await", "ids": ["tc_tp"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1,
            "summary": "Added it, with the setting you asked for." } ]
      ]
    },
    {
      "goalContains": "rig-setfail-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_sf", "name": "do", "approval": "free",
            "summary": "add a plugin that is not installed",
            "args": { "op": "add", "name": "Nothing Of This Name Exists",
                      "settings_structured": { "params": { "attack_db": 6, "sustain_db": -2 } } } },
          { "event": "await", "ids": ["tc_sf"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1, "summary": "One step did not land." } ]
      ]
    },
    {
      "goalContains": "rig-rowfail-marker",
      "rounds": [
        [ { "event": "round", "round": 1, "ofSoftCap": 20 },
          { "event": "tool_call", "id": "tc_rf", "name": "do", "approval": "free",
            "summary": "a change this rack will refuse",
            "args": { "op": "teleport_the_rack" } },
          { "event": "await", "ids": ["tc_rf"] } ],
        [ { "event": "done", "reason": "complete", "rounds": 1, "summary": "One step did not land." } ]
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
# --maps: the file the RIG writes after sampling a real third-party plugin's own controls (row T1,
# Sean's RULE 1). The mock reads it per request, so the order of startup does not matter. Without it
# /api/params/maps answers {} - which is how the rig passed four plugins that were sitting at their
# defaults in Logic: no map, no third-party dial, ever.
python3 "$ROOT/tools/host_rig/mock_server.py" --port "$PORT" --script "$SCRATCH/script.json" \
        --maps "$ECHOJAY_STATE_HOME/rig-maps.json" &
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

"$SCRATCH/host_rig" "$V2_COMP" "$LINK_COMP" "$PORT" "$V2_VST3"
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
