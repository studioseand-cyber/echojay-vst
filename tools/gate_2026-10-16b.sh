#!/bin/bash
# FULL GATE 26 (16 Oct 2026, the morning round). Sean rechecked ship_2026-10-16a in Logic, found
# twenty-two things, and then changed the approach: work to docs/AGENT_SCENARIO_MATRIX.md with a
# runner that loads real plugins, asserts on what the window draws, and keeps evidence.
#
# WHAT THIS GATE IS GATING, in one line each:
#   the blocker      an ask could be a dead end (no choices AND allowFreeText false). Typing always
#                    answers a chip-less ask; a typed answer that names a chip is sent as a tap; a
#                    typed answer that names none, with chips on screen, is a NEW INSTRUCTION that
#                    stops the waiting run and starts its own.
#   the read-back    an add carries `after`, so the plugin lands where the server asked - and both the
#                    agent's read-back AND the dial's own settled verification were reading an INDEX
#                    taken before the reorder. The first reported "not confirmed" over a correct value;
#                    the second REVERTED a write on an unrelated plugin ("slot 5 (Vertigo VSM-3)" for a
#                    write made to Renaissance Axx). By name, and by NodeID, respectively.
#   the wording      the landed line is split into the person's half and the model's; plain words and
#                    units in step details; only a `do` says "applied"; one waiting line draws one pair
#                    of buttons; the build bubble does not ride beside a run; one record per run.
#   the transcript   a reload MERGES what the store has not caught up with instead of appending it, and
#                    carries the agent snapshot with it; the interrupted line is drawn once.
#   the rack and GR  a stale mirror resolves toward the Link's own sidecar; no GR on a Link slot; the
#                    tile's badge is pushed live and its caption fits; the meter's two labels cannot
#                    overlap; check(gr) answers not_certified before it asks about silence; no instant
#                    figure is published at all; read-only runs count steps; the closing retires after
#                    an undo; Suggested Settings shows the landed value; the Link wires
#                    onSlotSettingsChanged at last.
#   AND A FIND       LinkShm::resolveDir probed writability with a file called ".probe" - the SAME NAME
#                    in every process - so two instances starting together could race, and the loser
#                    quietly took the TMPDIR fallback and ended up on a channel of its own. The
#                    out-of-process rig found it on its first run. Per-process probe now, with a retry
#                    and a loud log line if the fallback is ever taken.
#
# THE RUNNER IS PART OF THIS GATE: tools/link_host (one Link in its own process, holding real
# third-party plugins) and the matrix scenarios in tools/host_rig. The matrix ran RED on
# ship_2026-10-16a with evidence (docs/MATRIX_REPORT_2026-10-16.md) before any of this was fixed.
#
# FORCED delete of all SIX archives across THREE trees: ChainHost.h/.cpp, PluginEditor.h/.cpp,
# LinkShm.h, EJRigHook.h, EJAgentTools.h, EJAgentRunView.h, EJAgentProtocol.h, EJParamPlainNames.h,
# EchoJayParamApply.h, EJAgentClient.h/.cpp, EJAgentHome.h/.cpp, EJAgentExecutorDo.h/.cpp,
# EJAgentExecutorRead.cpp, LinkProcessor.cpp and viz/EedGrMeter.h all changed.
#
# NO SOURCE EDIT BETWEEN S1 AND THE END OF S4 (the gate-22 lesson): two guards compile the link side
# at TEST time, so "the binaries are already compiled" is false.
set -u
# ---- THE MACHINE HAS TO BE THIS GATE'S ALONE (the gate-17 lesson, 15 Oct) -------------------------
# Three guards did not regress 3x together: S4 ran beside Sean's Logic session and these guards
# LTO-compile a 300 MB archive at TEST time. B's local-server smoke runs are the same hazard from
# another direction, so this waits for them rather than producing a timeout that reads as a regression.
# WAIT ON THE LOAD, NOT ON A PID. My first cut of this blocked while B's node process EXISTED, and ten
# minutes in the machine was idle at load 1.8 with that process still alive and doing nothing. The hazard
# is CPU CONTENTION; load average is the measurement of it, and a pid is a proxy for the measurement that
# can be wrong in both directions. Measuring the proxy instead of the thing is the fault this tree has
# written down three times ("decisions on uncontrolled timing"), arriving in a new costume: I wrote a wait
# that measured who was present rather than what was happening.
#
# 3.0 on this machine leaves the guards most of the cores. Bounded at 20 minutes: a gate that waits for
# ever produces nothing, and a gate that runs under contention produces a timeout that reads as a
# regression - so after the bound it proceeds and the watchdog samples any stall.
for i in $(seq 1 120); do
  L=$(sysctl -n vm.loadavg | awk '{print $2}')
  BUSY=$(awk -v l="$L" 'BEGIN{ print (l+0 > 3.0) ? 1 : 0 }')
  if [ "$BUSY" -eq 1 ]; then
    [ $((i % 6)) -eq 1 ] && echo "waiting for the machine to go quiet (load $L, want <= 3.0)..."
    sleep 10
  else
    break
  fi
done
echo "the machine is quiet enough to gate (load $(sysctl -n vm.loadavg | awk '{print $2}'), threshold 3.0)$(pgrep -f 'real-closing.cjs' > /dev/null && echo '; B has a node process alive but idle' || echo '; B is clear')"
cd /Users/SeanD/echojay-vst
# $S WAS NEVER DEFINED IN HERE (14 Oct): it lived only in the shell that wrote this script, so S6 died on
# "S: unbound variable" after S1-S5 had passed. A script that references a path must define it.
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad

# ---- A GATE RUN ALONGSIDE A LOGIC SESSION IS NOT A GATE RUN (15 Oct 2026) --------------------------
# Gate 17 timed out on role_snapshot_guard (4340 s against a ~300 s baseline) and link_live_edit_guard,
# and level_match_guard took 833 s against ~280 s every previous gate. Three guards do not regress by 3x
# together: S4 ran while Sean was testing in Logic, and these guards LTO-compile a 300 MB archive at test
# time. A timeout is the one failure that looks like a product fault, so this refuses rather than spending
# an hour producing a red that means nothing. Same family as the standing "don't gate while EJ Map runs".
if pgrep -x "Logic Pro" > /dev/null; then
  echo "REFUSING TO GATE: Logic Pro is running. S4's two-sided guards compile a 300 MB archive each and"
  echo "  a shared machine turns that into a timeout that looks like a regression (gate 17, 15 Oct)."
  exit 1
fi
echo "Logic Pro is not running - the machine is this gate's alone"

echo "######## S1: forced delete of six archives, then relink (three trees) ########"
rm -f "/Users/SeanD/echojay-vst/build/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a"
rm -f "/Users/SeanD/echojay-vst/build/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a"
rm -f "/Users/SeanD/echojay-vst/build-guards/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a"
rm -f "/Users/SeanD/echojay-vst/build-guards/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a"
rm -f "/Users/SeanD/echojay-vst/build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a"
rm -f "/Users/SeanD/echojay-vst/build-release/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a"
echo "deleted; archives now present:"; ls -1 */EchoJay*_artefacts/Release/*SharedCode.a 2>/dev/null || echo "  (none - as expected)"
for T in build build-guards build-release; do
  echo "--- $T ---"
  cmake --build $T -j 4 --target EchoJay EchoJayLink || { echo "S1 FAILED in $T"; exit 1; }
done
# ---- A CONTENT MANIFEST, BECAUSE MTIME IS NOT THE THING THAT MATTERS (15 Oct 2026) --------------
# Gate 19 went red in S5 on a FALSE staleness: eighteen source files - exactly the set that differs
# from HEAD - had their mtimes bumped to one identical second MID-GATE, an hour after S3b had passed
# and long after S1 compiled them. The content never changed; the archive genuinely contained it.
#
# The provenance rule is mtime-based and blunt BY DESIGN, and that bluntness is right when the choice
# is between a blunt check and no check. But the thing it is trying to establish is "the binaries
# correspond to this source", and content answers that exactly while mtime only approximates it. So
# S1 records what it compiled, and the later stages can tell a bumped timestamp from a real edit.
shasum -a 256 $(find Source -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) | sort) \
  > "$S/gate_source_manifest.sha"
echo "S1 recorded a content manifest of $(wc -l < "$S/gate_source_manifest.sha" | tr -d ' ') source files"
echo "S1 OK"

echo "######## S2: guard targets (build-guards) ########"
cmake --build build-guards -j 4 || { echo "S2 FAILED"; exit 1; }
echo "S2 OK"

echo "######## S3: the four bundles (build-guards) ########"
cmake --build build-guards -j 4 --target EchoJay_AU EchoJay_VST3 EchoJayLink_AU EchoJayLink_VST3 || { echo "S3 FAILED"; exit 1; }
echo "S3 OK"

echo "######## S3b: archive provenance - the guards' link line must be newer than every header ########"
BAD=0
for A in "build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a" \
         "build-release/EchoJayLink_artefacts/Release/libEchoJay Link_SharedCode.a"; do
  if [ ! -f "$A" ]; then echo "  MISSING: $A"; BAD=1; continue; fi
  echo "  $A"
  N=$(find Source -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) -newer "$A" -print)
  if [ -n "$N" ]; then
    # MTIME SAYS NEWER. Ask CONTENT whether anything actually changed since S1 compiled it: a file
    # whose hash still matches the manifest was touched, not edited, and the archive is correct.
    CHANGED=""
    for f in $N; do
      WANT=$(grep -F " $f" "$S/gate_source_manifest.sha" 2>/dev/null | awk '{print $1}' | head -1)
      GOT=$(shasum -a 256 "$f" | awk '{print $1}')
      if [ -z "$WANT" ] || [ "$WANT" != "$GOT" ]; then CHANGED="$CHANGED $f"; fi
    done
    if [ -n "$CHANGED" ]; then
      echo "    NEWER *AND CHANGED* - this archive would disagree with the headers:"
      for f in $CHANGED; do echo "      $f"; done
      BAD=1
    else
      echo "    OK - $(echo $N | wc -w | tr -d ' ') file(s) have a newer mtime but IDENTICAL content to"
      echo "         what S1 compiled (checked by SHA-256 against the manifest), so nothing is stale."
    fi
  else
    echo "    OK - newer than every source in Source/"
  fi
done
[ "$BAD" -eq 0 ] || { echo "S3b FAILED - refusing to run ctest against a stale archive"; exit 1; }
echo "S3b OK"

echo "######## S4: ctest -L fast|plugins (watchdog armed) ########"
( while :; do
    for p in $(pgrep -f "build-guards/guards/" 2>/dev/null); do
      # ---- THE WATCHDOG HAS NEVER FIRED, AND THIS IS WHY (15 Oct 2026) ----------------------------
      # It read `ps -o etimes=`, which is a Linux/procps keyword. macOS ps has no `etimes`, so every
      # tick printed "ps: etimes: keyword not found" plus the whole keyword list into the gate log,
      # ET was that garbage, and `[ "$ET" -gt 300 ]` errored with "integer expression expected". It
      # has been doing that in every gate since it was written, and it never sampled a single stall -
      # including gate 17's two timeouts, which is exactly what it exists for.
      # macOS spells it `etime`, as [[dd-]hh:]mm:ss, so it needs parsing into seconds.
      ET=$(ps -o etime= -p "$p" 2>/dev/null | tr -d ' ')
      [ -n "$ET" ] || continue
      SECS=$(awk -v t="$ET" 'BEGIN{ sub(/-/,":",t); n=split(t,a,":"); s=0; for(i=1;i<=n;i++) s=s*60+a[i]+0; print s+0 }')
      [ -n "$SECS" ] || continue
      if [ "$SECS" -gt 300 ]; then
        N=$(ps -o comm= -p "$p" 2>/dev/null | xargs basename 2>/dev/null)
        if [ ! -f "$S/stall_$N.txt" ]; then
          echo "WATCHDOG: $N (pid $p) alive ${ET}s - sampling to $S/stall_$N.txt" >> "$S/watchdog.log"
          sample "$p" 5 -file "$S/stall_$N.txt" >/dev/null 2>&1 \
            || lldb -p "$p" -b -o "thread backtrace all" -o detach > "$S/stall_$N.txt" 2>&1
        fi
      fi
    done
    sleep 20
  done ) & WD=$!
( cd build-guards && ctest -L "fast|plugins" --output-on-failure )
S4=$?
kill $WD 2>/dev/null
echo "S4 EXIT=$S4"

echo "######## S5: link_state_guard (standalone two-sided) ########"
bash tools/link_state_guard/build_and_run.sh
S5=$?
echo "S5 EXIT=$S5"

echo "######## GATE SUMMARY ########"
echo "S4=$S4  S5=$S5"
if [ "$S4" -eq 0 ] && [ "$S5" -eq 0 ]; then echo "######## S6: the host rig (ask A) - real bundles, real editors, audio, the capability pass ########"
# Its own label, and a SKIP is reported as a skip: exit 77 means no GUI session, which is not a pass.
# EJ_RIG_LIVE points it at B's local server when one is up; without it the rig uses its mock and the
# capability rows skip themselves.
# THE PROBE, FIXED (14 Oct). gate10's was one `curl -m 3`, and it lost its race against B restarting their
# server - so S6 fell back to the MOCK, where every capability row skips, and would have printed GATE GREEN
# with C10 and L3 (the two rows this round fixed) never executed. A stage that quietly tests less than it
# claims is worse than one that fails, so there is no mock fallback here: it retries for 60 s across both
# ports and, finding nothing, FAILS the gate as NOT PROVEN.
# ---- HEALTH IS NOT THE TEST; A SERVED ROUND IS (16 Oct 2026) ------------------------------------
# AND THE TELL, FOR THE NEXT READER (B's FOR_A §52): the cause was Node's built-in fetch dispatching
# on a pooled HTTP/2 session to the provider that had been DESTROYED - ERR_HTTP2_INVALID_SESSION, no
# status code, nothing from the provider. /local/health's `modelFetchFailures` counter is what grows
# while it happens, and a restart is the cure. So if this stage ever says "rounds FAIL" again, read
# that counter before suspecting the account.
# The evening live pass went red on thirteen rows and the build had nothing to do with it: the server
# on 3211 answered /local/health with ok=true and agentOn=true all evening, and every single round came
# back `round_failed "The agent couldn't start, please try again."` A newer instance on 3213 answered
# perfectly. So the port is chosen by whether a REAL ROUND RUNS on it, and a port that only passes
# health is skipped with that said out loud.
RIGPORT=""
for _ in $(seq 1 12); do
  for P in ${EJ_LOCAL_PORT:-3213} 3211 3210; do
    curl -s -m 10 "http://127.0.0.1:$P/local/health" | grep -q '"ok"' || continue
    PT=$(curl -s -m 8 -X POST "http://127.0.0.1:$P/api/forge-dev-login" -H 'Content-Type: application/json' -d '{}' \
         | python3 -c 'import json,sys; print((json.load(sys.stdin) or {}).get("token") or "")' 2>/dev/null)
    [ -n "$PT" ] || { echo "S6: port $P has no dev login"; continue; }
    PR=$(curl -s -m 25 -X POST "http://127.0.0.1:$P/api/agent/start" -H 'Content-Type: application/json' \
           -H 'Accept: text/event-stream' -H "Authorization: Bearer $PT" \
           -d '{"goal":"gate probe: say hello","context":{"agentMode":true},"agentMode":true,"appVersion":"2.26.5","chatId":"gate-probe"}' \
         | head -c 400)
    case "$PR" in
      *round_failed*|*agent_mode_off*|"")
        echo "S6: port $P answers health but its ROUNDS FAIL - skipping it. It said:"; echo "$PR" | head -c 200; echo "" ;;
      *) RIGPORT="$P"; echo "S6: port $P serves a round"; break 2 ;;
    esac
  done
  echo "S6: waiting for a local server whose rounds run (3213, 3211, 3210)..."; sleep 5
done
if [ -z "$RIGPORT" ]; then
  echo "S6 NOT PROVEN: no local server SERVED A ROUND in 60 s, and this stage does not fall back to the mock -"
  echo "  the capability rows against the real model ARE the stage."
  echo ""
  echo "  S1-S5 ABOVE ARE THE VERDICTS THAT STAND: the archives, the guard suite, the four bundles and"
  echo "  the two-sided link guard all ran against this source. What is missing is the LIVE half, and"
  echo "  the reason is not in this tree: every model call on this account is failing right now, on the"
  echo "  agent route AND on plain chat (/api/chat-stream answers \"Internal server error\"), on both"
  echo "  server instances. Sean's install condition is \"S6 fully green\", so nothing is installed."
  echo ""
  echo "  GATE INCOMPLETE (not RED on the product): rerun this script when a round answers."
  exit 3
fi
echo "S6: live against B's local server on 127.0.0.1:$RIGPORT - the capability rows will run"
# ---- AND WHETHER THE AGENT ROUTE ACTUALLY ANSWERS (16 Oct 2026) ------------------------------------
# /local/health has said `agentOn:true` twice now while /api/agent/start answered `agent_mode_off`, and
# both times I found out forty minutes into a gate. One request, before the stage, so the log names it
# at the top rather than at the bottom.
GTOK=$(curl -s -m 8 -X POST "http://127.0.0.1:$RIGPORT/api/forge-dev-login" -H 'Content-Type: application/json' -d '{}' \
       | python3 -c 'import json,sys; print((json.load(sys.stdin) or {}).get("token") or "")' 2>/dev/null)
# THE BODY THE PLUGIN ACTUALLY SENDS. My first probe sent {goal, chatId} alone and was answered
# `agent_mode_off` - which I read as "their server is up without agent mode" and wrote to B twice. It
# is the BODY's own `agentMode` flag the route gates on, and the plugin has always sent it. A probe
# that does not send what the product sends is a probe that tests something else.
GSTART=$(curl -s -m 12 -X POST "http://127.0.0.1:$RIGPORT/api/agent/start" -H 'Content-Type: application/json' \
         -H 'Accept: text/event-stream' -H "Authorization: Bearer $GTOK" \
         -d '{"goal":"gate probe","context":{"agentMode":true},"agentMode":true,"appVersion":"2.26.5","chatId":"gate-probe"}' \
         | head -c 160)
echo "S6: /api/agent/start answers: $GSTART"
case "$GSTART" in
  *agent_mode_off*)
    echo "S6 NOT PROVEN: B's server is up and REFUSING the agent route (agent_mode_off). The capability"
    echo "  rows against the real model ARE this stage, so there is nothing here to pass. It is B's to"
    echo "  restart with agent mode on; FOR_B section 14 has the evidence. GATE RED."
    exit 1 ;;
esac
export EJ_RIG_LIVE="$RIGPORT"
# -V, NOT --output-on-failure (14 Oct): the latter prints a test's stdout ONLY when it fails, so a PASS
# left a log with nothing but ctest's summary - no rows, no "host_rig: GREEN" - and a check looking for
# that string called a fully green run not-green and refused to gate. The rig's per-row evidence is the
# point of keeping the log, so it is captured whatever the verdict.
ctest --test-dir build-guards -L rig -V > "$S/ej_gate26_s6.log" 2>&1
S6RC=$?
tail -40 "$S/ej_gate26_s6.log"
echo "S6 EXIT=$S6RC"
# WHAT COUNTS AS A SKIP, SAID EXACTLY (14 Oct). Two traps, both mine, both found before this script ran:
#
#   1. `grep -q "SKIP"` matched the RIG'S OWN per-row "  SKIP" lines, which exist because a row that could not
#      run used to print "ok  (skipped)" and pad the pass count. So a genuine S6 FAILURE that happened to
#      contain one skipped row would have been reclassified as "skipped (loudly)" and the gate would have gone
#      on to print GATE GREEN. The no-GUI skip is ctest's OWN verdict on the whole test, nothing else.
#   2. ctest with SKIP_RETURN_CODE 77 returns 0 for a skipped test, so a no-GUI run would have reached
#      "S6 OK" quietly - which is the exact thing Sean ruled against: "make S6 skip loudly, not pass".
if grep -qE "^ *[0-9]+/[0-9]+ Test +#[0-9]+: host_rig .*\*+Skipped" "$S/ej_gate26_s6.log"; then
  echo "S6 SKIPPED (loudly): ctest skipped the whole rig - no GUI session. That is NOT a pass, so GATE RED."
  exit 1
fi
if [ $S6RC -ne 0 ]; then echo "S6 FAILED"; exit 1; fi
# Rows that could not run are named loudly in the summary. The rig owns pass/fail; this makes its skips
# impossible to miss in a gate log, because today a skip counted as a pass for an entire round.
if grep -qE "skipped\)" "$S/ej_gate26_s6.log"; then
  echo "S6 NOTE - the rig reports skipped row(s):"; grep -E "^  SKIP|[0-9]+ leg\(s\) could not run" "$S/ej_gate26_s6.log"
fi
echo "S6 OK"

echo "GATE GREEN"; else
  # ---- A RED GATE MUST EXIT RED (17 Oct 2026) ---------------------------------------------------
  # It printed "GATE RED" and exited 0, so every caller that trusted the exit code was told the gate
  # had passed. Today's chain only refused to install because a SECOND check - counting S6's verdict
  # lines, added this morning after a red half got past a `grep -q` - disagreed with it. Two
  # independent checks saved an install that one of them was actively lying about.
  echo "GATE RED"
  exit 1
fi
