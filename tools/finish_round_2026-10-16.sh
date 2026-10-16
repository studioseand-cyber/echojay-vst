#!/bin/bash
# ============================================================================================
#  FINISH THE 16 OCT ROUND: the gate, then the install - in one command.
#
#  Why this exists: the deterministic matrix is GREEN (107 passed, 0 failed) and the live half
#  proved eight of its thirteen rows at five attempts out of five - and then every model call on
#  this account started failing, on both local-agent-server instances and on plain chat as well
#  as the agent route. S6 IS the live rig, Sean's install condition is "S6 fully green", so the
#  round stopped one stage short with nothing installed.
#
#  Run this when a round answers again. It checks that first, refuses in one line if not, and
#  otherwise runs the full gate and - only on a green gate - places and installs the ship.
#
#  Logic must be closed (the install refuses otherwise).
# ============================================================================================
set -u
cd /Users/SeanD/echojay-vst || exit 2
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad

echo "######## is the model answering? ########"
OK=""
for P in 3213 3211 3210; do
  curl -s -m 8 "http://127.0.0.1:$P/local/health" | grep -q '"ok"' || continue
  T=$(curl -s -m 8 -X POST "http://127.0.0.1:$P/api/forge-dev-login" -H 'Content-Type: application/json' -d '{}' \
      | python3 -c 'import json,sys; print((json.load(sys.stdin) or {}).get("token") or "")' 2>/dev/null)
  [ -n "$T" ] || continue
  R=$(curl -s -m 25 -X POST "http://127.0.0.1:$P/api/agent/start" -H 'Content-Type: application/json' \
        -H 'Accept: text/event-stream' -H "Authorization: Bearer $T" \
        -d '{"goal":"finish-script probe: say hello","context":{"agentMode":true},"agentMode":true,"appVersion":"2.26.5","chatId":"finish-probe"}' \
      | head -c 400)
  case "$R" in *round_failed*|*agent_mode_off*|"") echo "  port $P: health ok, rounds FAIL"; continue ;; esac
  OK="$P"; echo "  port $P: serves a round"; break
done
if [ -z "$OK" ]; then
  echo ""
  echo "REFUSED: no local server serves a round, so S6 cannot run and the install condition"
  echo "  (\"S6 fully green\") cannot be met. Nothing has been touched. Try again when the model"
  echo "  is answering - one way to tell in a second:"
  echo "    curl -s -m 8 -X POST http://127.0.0.1:3213/api/forge-dev-login -H 'Content-Type: application/json' -d '{}'"
  exit 3
fi

echo "######## the gate ########"
export EJ_LOCAL_PORT="$OK"
bash tools/gate_2026-10-16b.sh 2>&1 | tee "$S/ej_gate26_finish.log"
GRC=${PIPESTATUS[0]}
echo "GATE EXIT=$GRC"
if [ "$GRC" -ne 0 ]; then
  echo "The gate did not go green, so nothing is installed. Its log: $S/ej_gate26_finish.log"
  exit 1
fi

echo "######## place and install ########"
bash tools/ship_and_install_2026-10-16b.sh
