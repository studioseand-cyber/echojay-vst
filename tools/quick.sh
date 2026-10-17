#!/bin/bash
# =============================================================================
#  QUICK — the fast tier (Sean's new way of working, 17 Oct 2026)
# =============================================================================
#  "Two build tiers. QUICK: incremental build, install AU and VST3 to the normal
#   places, run the S4 guards plus only the matrix rows that touch the change,
#   then tell me. Target under 15 minutes, and tell me the real time. FULL gate
#   only overnight and before anything goes to beta."
#
#  Usage:   tools/quick.sh "<what changed>" [GUARDS] [ROWS]
#    GUARDS  comma-separated ctest -R pattern, or "none"   (default: the agent set)
#    ROWS    comma-separated matrix row ids, or "none"      (default: none)
#
#  Example: tools/quick.sh "the §25 inventory fix" "agent_client_guard|plain_names_guard" "RP8K"
#
#  WHAT QUICK IS NOT. It is not a gate and it never claims to be: no forced
#  archive delete, no three trees, no full matrix, no live pass. It builds what
#  changed, installs, runs the guards you name and the rows you name, and prints
#  the real elapsed time. The FULL gate still runs overnight and before beta, and
#  this script says so in its own summary so a QUICK log can never be mistaken
#  for one.
#
#  AND IT REFUSES TO INSTALL UNDER A LIVE HOST, like every other install road
#  here: Logic holds the old binary open and the copy would be half-read.
# =============================================================================
set -u
cd /Users/SeanD/echojay-vst || exit 2
T0=$(date +%s)
WHAT="${1:-an unnamed change}"
GUARDS="${2:-agent_client_guard|plain_names_guard}"
ROWS="${3:-none}"
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad
mkdir -p "$S"
LOG="$S/quick_$(date +%H%M%S).log"
say() { printf '%s\n' "$*" | tee -a "$LOG"; }
mins() { printf '%dm %02ds' $(( ($1) / 60 )) $(( ($1) % 60 )); }

say "######## QUICK: $WHAT"
say "HEAD: $(git log --oneline -1)"

if pgrep -x "Logic Pro" > /dev/null; then
  say "REFUSING TO INSTALL: Logic Pro is running - it holds the old binary open."
  say "Close Logic and run this again (the build below is still useful, so it runs)."
  LOGIC_OPEN=1
else
  LOGIC_OPEN=0
fi

say ""; say "---- 1. incremental build (-j 4, the four bundles) ----"
B0=$(date +%s)
if ! cmake --build build-guards -j 4 --target EchoJay_AU EchoJay_VST3 EchoJayLink_AU EchoJayLink_VST3 \
     >> "$LOG" 2>&1; then
  say "BUILD FAILED - nothing installed. The errors:"
  grep -E "error:" "$LOG" | tail -8 | tee -a "$LOG"
  say "elapsed: $(mins $(( $(date +%s) - T0 )))"; exit 1
fi
say "built in $(mins $(( $(date +%s) - B0 )))"

say ""; say "---- 2. the guards ----"
G0=$(date +%s)
if [ "$GUARDS" = "none" ]; then
  say "(skipped by request)"
  GRC=0
else
  cmake --build build-guards -j 4 --target $(echo "$GUARDS" | tr '|' ' ') >> "$LOG" 2>&1
  ( cd build-guards && ctest -R "$GUARDS" --output-on-failure ) >> "$LOG" 2>&1
  GRC=$?
  grep -E "tests passed|The following tests FAILED" "$LOG" | tail -2 | tee -a "$LOG"
  [ "$GRC" -eq 0 ] || grep -E "^\s*FAIL " "$LOG" | tail -6 | cut -c1-170 | tee -a "$LOG"
fi
say "guards in $(mins $(( $(date +%s) - G0 )))  (exit $GRC)"

say ""; say "---- 3. the matrix rows that touch the change ----"
R0=$(date +%s)
RRC=0
if [ "$ROWS" = "none" ]; then
  say "(none named - say which rows touch the change and they run here)"
else
  V2="/Users/SeanD/echojay-vst/build-guards/EchoJay_artefacts/Release/AU/EchoJay V2.component"
  LK="/Users/SeanD/echojay-vst/build-guards/EchoJayLink_artefacts/Release/AU/EchoJay Link.component"
  V3="/Users/SeanD/echojay-vst/build-guards/EchoJay_artefacts/Release/VST3/EchoJay V2.vst3"
  EJ_RIG_ONLY="$ROWS" EJ_RIG_LIVE= EJ_RIG_TOKEN= \
    bash tools/host_rig/build_and_run.sh "$V2" "$LK" "" "$V3" > "$S/quick_rows.log" 2>&1
  RRC=$?
  grep -E "FILTERED RUN|  (SCEN|ROW) " "$S/quick_rows.log" | sed -e 's/.*host_rig\[[0-9:]*\] //' \
    | cut -c1-200 | tee -a "$LOG"
  grep -E "  (SCEN|ROW) .*FAIL" -A 1 "$S/quick_rows.log" | sed -e 's/.*host_rig\[[0-9:]*\] //' \
    | grep -vE "^--|evidence" | cut -c1-230 | tee -a "$LOG"
fi
say "rows in $(mins $(( $(date +%s) - R0 )))  (exit $RRC)"

say ""; say "---- 4. install ----"
if [ "$LOGIC_OPEN" = "1" ]; then
  say "NOT INSTALLED: Logic Pro is open. Close it and run tools/quick.sh again."
elif [ "$GRC" -ne 0 ] || [ "$RRC" -ne 0 ]; then
  say "NOT INSTALLED: something above is red, and QUICK installs only on green."
else
  bash tools/install_local.sh build-guards >> "$LOG" 2>&1 && say "installed AU + VST3 from build-guards" \
    || { say "INSTALL FAILED - see $LOG"; }
  killall -9 AUHostingService 2>/dev/null; sleep 1; killall -9 AUHostingService 2>/dev/null
  say "AUHostingService killed twice (the standing rule)"
  for P in "Components/EchoJay V2.component/Contents/MacOS/EchoJay V2" \
           "Components/EchoJay Link.component/Contents/MacOS/EchoJay Link"; do
    U=$(dwarfdump --uuid "$HOME/Library/Audio/Plug-Ins/$P" 2>/dev/null | grep arm64 | awk '{print $2}')
    say "  live $(basename "$P"): $U"
  done
fi

EL=$(( $(date +%s) - T0 ))
say ""
say "######## QUICK DONE in $(mins $EL) — target was 15m"
say "This is NOT a gate: no forced archive delete, no three trees, no full matrix,"
say "no live pass. The FULL gate runs overnight and before anything goes to beta."
say "log: $LOG"
[ "$GRC" -eq 0 ] && [ "$RRC" -eq 0 ] || exit 1
