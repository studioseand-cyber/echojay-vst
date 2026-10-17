#!/bin/bash
# =============================================================================
#  THE PLAN CONSISTENCY SIM (Sean's step 7.4(B), 16 Oct 2026)
# =============================================================================
#  "Three different plans for the same request tonight (a pick ask, an add, a
#   3-slot rebuild); add a consistency sim."
#
#  WHAT IT MEASURES, AND WHOSE IT IS. The plan is the SERVER's: the model is
#  handed the rack and decides the ops. My side cannot make it consistent; it
#  can only measure how inconsistent it is, with the same request, the same
#  rack and the same context every time, so that the variance is a number in
#  FOR_B rather than three anecdotes from one evening in Logic.
#
#  WHY IT IS NOT A MATRIX ROW TONIGHT. A row that fails when the model picks a
#  different shape would fail the gate for something no change of mine can fix,
#  and a row that passes whatever comes back is not a test. It is a REPORT, so
#  it is a script that prints a tally. Folding it into the rig as a reporting
#  row is NEXT_ROUND_QUEUE item 12.
#
#  The rack it asks about always already HOLDS two plugins, because that is the
#  whole question: given a chain that exists, does a tweak request adjust it or
#  replace it?
#
#  Usage: plan_consistency_sim.sh [port] [repeats]
# =============================================================================
set -u
PORT="${1:-3213}"
N="${2:-6}"
GOAL="${EJ_SIM_GOAL:-make the kick punchier}"
OUT=$(mktemp -d "${TMPDIR:-/tmp}/plan-sim.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

TOK=$(curl -s -m 8 -X POST "http://127.0.0.1:$PORT/api/forge-dev-login" \
        -H 'Content-Type: application/json' -d '{}' \
      | python3 -c 'import json,sys; print((json.load(sys.stdin) or {}).get("token") or "")' 2>/dev/null)
if [ -z "$TOK" ]; then echo "REFUSED: no dev login on 127.0.0.1:$PORT"; exit 2; fi

# THE SAME RACK EVERY TIME, and one that is not empty: a Transient Master and a
# Renaissance Axx, which is what the Kick's Link was holding in Logic when the
# model proposed replacing both with three EchoJay slots.
CTX='{"agentMode":true,
      "trackName":"88_7even Kick_2_01",
      "chainSlots":[{"n":1,"name":"Transient Master"},{"n":2,"name":"Renaissance Axx"}],
      "rackSlots":[{"n":1,"name":"Transient Master"},{"n":2,"name":"Renaissance Axx"}],
      "inventory":{"count":3,"names":["Transient Master","Renaissance Axx","EchoJay Compressor"]}}'

echo "####### plan consistency: \"$GOAL\" x $N, on a rack that already holds 2 plugins #######"
for i in $(seq 1 "$N"); do
  R=$(curl -s -m 60 -X POST "http://127.0.0.1:$PORT/api/agent/start" \
        -H 'Content-Type: application/json' -H 'Accept: text/event-stream' \
        -H "Authorization: Bearer $TOK" \
        -d "{\"goal\":$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$GOAL"),\"context\":$(echo "$CTX" | tr -d '\n'),\"agentMode\":true,\"appVersion\":\"2.26.5\",\"chatId\":\"sim-$i-$$\"}" \
      | head -c 20000)
  echo "$R" > "$OUT/plan_sim_$i.sse"
  # CLASSIFY BY WHAT THE FRAMES ASK FOR, not by prose: a pick ask (choices on an
  # await), an add (one add op), or a rebuild (a build/several adds at once).
  python3 - "$i" "$OUT" <<'PY'
import json, sys, re
i, out = sys.argv[1], sys.argv[2]
raw = open(f"{out}/plan_sim_{i}.sse", encoding="utf-8", errors="replace").read()
frames = []
for line in raw.splitlines():
    if line.startswith("data:"):
        try: frames.append(json.loads(line[5:].strip()))
        except Exception: pass
kinds = [f.get("type") or f.get("event") or "" for f in frames]
adds = 0; builds = 0; choices = 0; names = []
for f in frames:
    blob = json.dumps(f)
    adds   += len(re.findall(r'"op"\s*:\s*"add', blob))
    builds += len(re.findall(r'"op"\s*:\s*"(build|replace|set_chain)', blob))
    if f.get("choices"): choices = max(choices, len(f["choices"]))
    for m in re.findall(r'"(?:plugin|name|label)"\s*:\s*"([^"]{2,40})"', blob):
        if m not in names: names.append(m)
if builds or adds >= 3: shape = "REBUILD/MULTI-ADD (%d add, %d build)" % (adds, builds)
elif choices:          shape = "PICK ASK (%d choices)" % choices
elif adds == 1:        shape = "ONE ADD"
elif adds:             shape = "%d ADDS" % adds
elif "round_failed" in raw: shape = "ROUND FAILED"
else:                  shape = "NO OPS (talk only)"
print(f"  attempt {i}: {shape}   frames={len(frames)}  first names={names[:4]}")
PY
done
echo "####### done - the tally above is the measurement; the shapes are the server's to settle #######"
