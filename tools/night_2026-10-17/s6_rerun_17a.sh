#!/bin/bash
# S6 ON ITS OWN, after a harness fix, with S1-S5 already green on this source.
# Legitimate because host_rig.mm is not Source/: it is compiled at TEST time by
# build_and_run.sh and links the archive S1 built, so the bundles under test are
# unchanged. The rule it must not break is the other one - nothing installs
# unless S1-S5 are green in THIS run of the gate and S6 is green here.
cd /Users/SeanD/echojay-vst || exit 2
set -u
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad
N="${1:-1}"
if pgrep -x "Logic Pro" > /dev/null; then echo "REFUSED: Logic Pro is running"; exit 1; fi
PORT=""
for P in 3213 3211 3210; do
  curl -s -m 10 "http://127.0.0.1:$P/local/health" | grep -q '"ok"' || continue
  T=$(curl -s -m 8 -X POST "http://127.0.0.1:$P/api/forge-dev-login" -H 'Content-Type: application/json' -d '{}' \
      | python3 -c 'import json,sys; print((json.load(sys.stdin) or {}).get("token") or "")' 2>/dev/null)
  [ -n "$T" ] || continue
  R=$(curl -s -m 25 -X POST "http://127.0.0.1:$P/api/agent/start" -H 'Content-Type: application/json' \
        -H 'Accept: text/event-stream' -H "Authorization: Bearer $T" \
        -d '{"goal":"s6 probe: say hello","context":{"agentMode":true},"agentMode":true,"appVersion":"2.26.5","chatId":"s6-probe"}' | head -c 300)
  case "$R" in *round_failed*|*agent_mode_off*|"") echo "  $P: rounds fail" ;; *) PORT="$P"; break ;; esac
done
[ -n "$PORT" ] || { echo "S6 NOT PROVEN: no server serves a round"; exit 3; }
echo "S6 rerun on 127.0.0.1:$PORT, EJ_LIVE_REPEATS=$N"
EJ_RIG_LIVE="$PORT" EJ_LIVE_REPEATS="$N" ctest --test-dir build-guards -L rig -V > "$S/s6_rerun.log" 2>&1
RC=$?
echo "S6 RERUN EXIT=$RC"
grep -E "==== host_rig:" "$S/s6_rerun.log"
grep -E "SCEN .*(FAIL|SKIP)|ROW .*FAIL" "$S/s6_rerun.log" | sed -e 's/.*host_rig\[[0-9:]*\] //' | cut -c1-150
