#!/bin/bash
# =============================================================================================
#  FINISH THE B3 ROUND (22 Oct 2026) - the one command for "Logic is closed".
#
#  His order for today, step 4: *"When I say Logic is closed: install the B3 build, then run S6
#  live and mock end to end and report."*
#
#  WHAT IT DOES, AND THE ONE PLACE IT DEPARTS FROM HIS WORDING. ChainHost.h changed, which is a
#  header every guard compiles, so the guard suite runs BEFORE the install rather than after it:
#  an install is the one step that cannot be taken back without a second install, and the suite is
#  how we know the other 85 claims still hold. Everything after that is his order as written.
#
#    1  refuse if a host is open, or if the disk cannot take a universal relink
#    2  forced delete of the SIX archives across THREE trees (ChainHost.h + EJAgentHome.* changed)
#    3  relink the three trees, the guard targets, and the four bundles AND the two AAX plugins
#       (Pro Tools gets the same source as Logic this time)
#    4  S4: ctest -L "fast|plugins"      S5: link_state_guard
#    5  install (AU + VST3, both plugins) and print the UUIDs
#    6  S6 on the MOCK road, then S6 on the LIVE road (B's local server), each logged in full
#
#  Usage: bash tools/finish_b3_2026-10-22.sh     (logs land in the session scratchpad)
# =============================================================================================
set -u
cd /Users/SeanD/echojay-vst
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad
STAMP=$(date +%H%M)

for H in "Logic Pro" "Pro Tools"; do
  if pgrep -x "$H" > /dev/null; then
    echo "REFUSING: $H is running. This script builds, installs and then drives real editors in the"
    echo "  rig; every one of those is something that must not happen under a live host."
    exit 1
  fi
done
FREEMB=$(df -m /System/Volumes/Data | awk 'NR==2 {print $4}')
if [ "${FREEMB:-0}" -lt 30720 ]; then
  echo "REFUSING: only $((FREEMB / 1024)) GB free; a universal + LTO relink needs 30 GB (the 18 Oct lesson)."
  exit 1
fi
echo "no host is open; $((FREEMB / 1024)) GB free"

echo "######## 1. forced delete of six archives (ChainHost.h is a header every target compiles) ########"
for T in build build-guards build-release; do
  rm -f "$T/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a" \
        "$T/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a"
done
ls -1 */EchoJay*_artefacts/Release/*SharedCode.a 2>/dev/null || echo "  (none left, as expected)"

echo "######## 2. relink the three trees ########"
for T in build build-guards build-release; do
  echo "--- $T ---"
  cmake --build "$T" -j 4 --target EchoJay EchoJayLink || { echo "RELINK FAILED in $T"; exit 1; }
done
shasum -a 256 $(find Source -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) | sort) \
  > "$S/b3_source_manifest.sha"
echo "recorded a content manifest of $(wc -l < "$S/b3_source_manifest.sha" | tr -d ' ') source files"

echo "######## 3. the guard targets, the four bundles, and AAX for Pro Tools ########"
cmake --build build-guards -j 4 || { echo "GUARD TARGETS FAILED"; exit 1; }
cmake --build build-guards -j 4 --target EchoJay_AU EchoJay_VST3 EchoJayLink_AU EchoJayLink_VST3 \
      EchoJay_AAX EchoJayLink_AAX || { echo "BUNDLES FAILED"; exit 1; }

echo "######## 4. S4 and S5 ########"
( cd build-guards && ctest -L "fast|plugins" --output-on-failure ) > "$S/b3_s4_$STAMP.log" 2>&1
S4=$?
tail -12 "$S/b3_s4_$STAMP.log"
bash tools/link_state_guard/build_and_run.sh > "$S/b3_s5_$STAMP.log" 2>&1
S5=$?
tail -4 "$S/b3_s5_$STAMP.log"
echo "S4=$S4 S5=$S5"
[ "$S4" -eq 0 ] && [ "$S5" -eq 0 ] || { echo "REFUSING TO INSTALL: the suite is not green. Logs above."; exit 1; }

echo "######## 5. install, and name what a host will load ########"
tools/install_local.sh build-guards || { echo "INSTALL REPORTED PROBLEMS"; exit 1; }
killall -9 AUHostingService 2>/dev/null
echo "AUHostingService cleared"

echo "######## 6. S6 on the mock road ########"
bash tools/host_rig/build_and_run.sh > "$S/b3_rig_mock_$STAMP.log" 2>&1
RIGM=$?
grep -E "^==== host_rig|FAIL " "$S/b3_rig_mock_$STAMP.log" | tail -25
echo "mock rig exit=$RIGM  (log: $S/b3_rig_mock_$STAMP.log)"

echo "######## 6b. S6 on the live road ########"
RIGPORT=""
for P in 3213 3211 3210; do
  curl -s -m 10 "http://127.0.0.1:$P/local/health" | grep -q '"ok"' || continue
  PT=$(curl -s -m 8 -X POST "http://127.0.0.1:$P/api/forge-dev-login" -H 'Content-Type: application/json' -d '{}' \
       | python3 -c 'import json,sys; print((json.load(sys.stdin) or {}).get("token") or "")' 2>/dev/null)
  [ -n "$PT" ] || continue
  PR=$(curl -s -m 25 -X POST "http://127.0.0.1:$P/api/agent/start" -H 'Content-Type: application/json' \
         -H 'Accept: text/event-stream' -H "Authorization: Bearer $PT" \
         -d '{"goal":"gate probe: say hello","context":{"agentMode":true},"agentMode":true,"appVersion":"2.26.5","chatId":"gate-probe"}' \
       | head -c 400)
  case "$PR" in
    *round_failed*|*agent_mode_off*|"") echo "  port $P answers health but its ROUNDS FAIL - skipping it" ;;
    *) RIGPORT="$P"; echo "  port $P serves a round"; break ;;
  esac
done
if [ -z "$RIGPORT" ]; then
  echo "S6 LIVE NOT PROVEN: no local server served a round. The mock half above stands; the live half"
  echo "  needs one of B's servers answering. Nothing here is a verdict on the product."
  exit 3
fi
EJ_RIG_LIVE="$RIGPORT" bash tools/host_rig/build_and_run.sh > "$S/b3_rig_live_$STAMP.log" 2>&1
RIGL=$?
grep -E "^==== host_rig|FAIL " "$S/b3_rig_live_$STAMP.log" | tail -25
echo "live rig exit=$RIGL  (log: $S/b3_rig_live_$STAMP.log)"
echo "######## DONE: S4=$S4 S5=$S5 mock=$RIGM live=$RIGL ########"
