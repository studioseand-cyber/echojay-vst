#!/bin/bash
# =============================================================================
#  QUICK - the fast tier (Sean's way of working, 17 Oct 2026; re-cut 18 Oct)
# =============================================================================
#  "Two build tiers. QUICK: incremental build, install AU and VST3 to the normal
#   places, run the S4 guards plus only the matrix rows that touch the change,
#   then tell me. Target under 15 minutes, and tell me the real time. FULL gate
#   only overnight and before anything goes to beta."
#
#  ---- 18 OCT 2026, HIS THREE RULES FOR MAKING IT FASTER ---------------------
#  1. "QUICK builds without LTO (keep LTO for the overnight full gate, which
#      checks the real ship build)."  QUICK has its own tree, configured once
#      with EJ_GUARDS_NO_LTO=ON. The gate's three trees are untouched, so the
#      thing that goes to beta is still built and gated exactly as it ships.
#      The same tree also builds for THIS Mac's architecture only (arm64): his
#      Logic is arm64, and a universal build compiles everything twice to
#      produce a slice nothing here loads. Both deviations are named in the
#      banner of every run, never silent.
#  2. "QUICK installs right after the build, then runs the guards in the
#      background; if a guard goes red, tell me. Don't make me wait for guards
#      before I can test."  So the order is build -> install -> guards, and the
#      guard verdict lands in a file this script names. A red is reported to him
#      by whoever ran the script - that is the price of not waiting, and it is
#      his call to pay it.
#  3. "Only run ui_guard when the change is UI layout."  ui_guard links the
#      whole editor; it is the slowest guard in the tree by a long way. Naming
#      it is now a deliberate act, and this script says so when it sees it.
#
#  Usage:   tools/quick.sh "<what changed>" [GUARDS] [ROWS]
#    GUARDS  ctest -R pattern (| separated), or "none"   (default: the agent set)
#    ROWS    matrix row ids, or "none"                   (default: none)
#  Env:
#    NO_INSTALL=1      build (and guard) without installing
#    EJ_QUICK_TREE     which build directory to use      (default: build-quick)
#    EJ_QUICK_ARCH     architectures for a fresh tree     (default: arm64)
#    EJ_GUARDS_FG=1    run the guards in the foreground, as QUICK used to
#
#  WHAT QUICK IS NOT. It is not a gate and it never claims to be: no forced
#  archive delete, no three trees, no full matrix, no live pass, and from today
#  no LTO and one architecture. It builds what changed, installs it, starts the
#  guards you named and prints the real elapsed time.
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
TREE="${EJ_QUICK_TREE:-build-quick}"
ARCH="${EJ_QUICK_ARCH:-arm64}"
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad
mkdir -p "$S"
STAMP=$(date +%H%M%S)
LOG="$S/quick_$STAMP.log"
GLOG="$S/quick_guards_$STAMP.log"
GVERDICT="$S/quick_guards_$STAMP.verdict"
say() { printf '%s\n' "$*" | tee -a "$LOG"; }
mins() { printf '%dm %02ds' $(( ($1) / 60 )) $(( ($1) % 60 )); }

say "######## QUICK: $WHAT"
say "HEAD: $(git log --oneline -1)"
# The banner states what the tree ACTUALLY is, read off its own cache - not what QUICK's default
# would have been. An override (EJ_QUICK_TREE=build-guards, to use a warm tree while he waits) is
# a legitimate thing to do, and a banner that claimed "no LTO" over an LTO tree would be a log
# that lies about its own evidence.
if [ -f "$TREE/CMakeCache.txt" ]; then
  TREE_LTO=$(grep -E "^EJ_GUARDS_NO_LTO:BOOL=ON" "$TREE/CMakeCache.txt" > /dev/null && echo "no LTO" || echo "LTO")
  TREE_ARCH=$(grep -E "^CMAKE_OSX_ARCHITECTURES:" "$TREE/CMakeCache.txt" | cut -d= -f2)
  say "tree: $TREE   ($TREE_LTO, $TREE_ARCH)"
else
  say "tree: $TREE   (will be configured now: no LTO, $ARCH)"
fi
case "$GUARDS" in *ui_guard*) say "note: ui_guard was named - it links the whole editor, so only for UI layout changes";; esac

if pgrep -x "Logic Pro" > /dev/null; then
  say "Logic Pro is OPEN: this run will build and guard but NOT install."
  LOGIC_OPEN=1
else
  LOGIC_OPEN=0
fi

# ---- 0. the tree, configured once ------------------------------------------------------------
if [ ! -f "$TREE/CMakeCache.txt" ]; then
  say ""; say "---- 0. configuring $TREE once (no LTO, $ARCH) ----"
  C0=$(date +%s)
  if ! cmake -S . -B "$TREE" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
        -DEJ_BUILD_GUARDS=ON -DEJ_GUARDS_NO_LTO=ON -DJUCE_PATH=/Users/SeanD/JUCE \
        >> "$LOG" 2>&1; then
    say "CONFIGURE FAILED - see $LOG"; exit 1
  fi
  say "configured in $(mins $(( $(date +%s) - C0 )))  (the first build in a fresh tree is a FULL one)"
fi

say ""; say "---- 1. incremental build (-j 4, the four bundles) ----"
B0=$(date +%s)
if ! cmake --build "$TREE" -j 4 --target EchoJay_AU EchoJay_VST3 EchoJayLink_AU EchoJayLink_VST3 \
     >> "$LOG" 2>&1; then
  say "BUILD FAILED - nothing installed. The errors:"
  grep -E "error:" "$LOG" | tail -8 | tee -a "$LOG"
  say "elapsed: $(mins $(( $(date +%s) - T0 )))"; exit 1
fi
say "built in $(mins $(( $(date +%s) - B0 )))"

# ---- 2. install, BEFORE the guards (his rule 2) ----------------------------------------------
# A guard that goes red after this has installed something he is already testing. That is the
# trade he asked for in words, and the alternative he rejected was waiting ten minutes to type a
# sentence into Logic. The red is REPORTED, loudly, and the revert road is one line below.
say ""; say "---- 2. install ----"
if pgrep -x "Logic Pro" > /dev/null; then LOGIC_OPEN=1; fi
if [ "${NO_INSTALL:-0}" = "1" ]; then
  say "NOT INSTALLED: NO_INSTALL=1 was asked for. The bundles are in $TREE; install with"
  say "  bash tools/install_local.sh $TREE   (Logic closed)"
elif [ "$LOGIC_OPEN" = "1" ]; then
  say "NOT INSTALLED: Logic Pro is open. Close it and run tools/quick.sh again."
else
  bash tools/install_local.sh "$TREE" >> "$LOG" 2>&1 && say "installed AU + VST3 from $TREE" \
    || say "INSTALL FAILED - see $LOG"
  killall -9 AUHostingService 2>/dev/null; sleep 1; killall -9 AUHostingService 2>/dev/null
  say "AUHostingService killed twice (the standing rule)"
  for P in "Components/EchoJay V2.component/Contents/MacOS/EchoJay V2" \
           "Components/EchoJay Link.component/Contents/MacOS/EchoJay Link"; do
    U=$(dwarfdump --uuid "$HOME/Library/Audio/Plug-Ins/$P" 2>/dev/null | grep -E "arm64" | awk '{print $2}')
    H=$(shasum -a 256 "$HOME/Library/Audio/Plug-Ins/$P" 2>/dev/null | cut -c1-16)
    say "  live $(basename "$P"): $U  sha=$H"
  done
fi

# ---- 3. the guards, in the background (his rule 2) -------------------------------------------
# A GUARD THAT DID NOT COMPILE IS NOT A GUARD THAT PASSED (18 Oct 2026): the build's exit code is
# checked here, because a harness with a compile error otherwise leaves the PREVIOUS binary for
# ctest to run and the summary reads "100% tests passed" over legs that were never built.
say ""; say "---- 3. the guards ----"
if [ "$GUARDS" = "none" ]; then
  say "(skipped by request)"
else
  RUNNER=$(cat <<RUN
set -u
cd /Users/SeanD/echojay-vst
G0=\$(date +%s)
if ! cmake --build "$TREE" -j 4 --target \$(echo "$GUARDS" | tr '|' ' ') >> "$GLOG" 2>&1; then
  { echo "RED: THE GUARD BUILD FAILED - the binaries are STALE, so nothing would be evidence."
    grep -E "error:|No rule to make target" "$GLOG" | tail -8; } > "$GVERDICT"
  exit 1
fi
( cd "$TREE" && ctest -R "$GUARDS" --output-on-failure ) >> "$GLOG" 2>&1
RC=\$?
{ if [ \$RC -eq 0 ]; then echo "GREEN after \$(( \$(date +%s) - G0 ))s: $GUARDS"; else echo "RED after \$(( \$(date +%s) - G0 ))s:"; fi
  grep -E "tests passed|The following tests FAILED" "$GLOG" | tail -2
  [ \$RC -eq 0 ] || grep -E "^\s*FAIL " "$GLOG" | tail -8 | cut -c1-200; } > "$GVERDICT"
exit \$RC
RUN
)
  if [ "${EJ_GUARDS_FG:-0}" = "1" ]; then
    bash -c "$RUNNER"; cat "$GVERDICT" | tee -a "$LOG"
  else
    nohup bash -c "$RUNNER" > /dev/null 2>&1 &
    say "started in the background: $GUARDS"
    say "  verdict: $GVERDICT      (read it; a RED must be reported to Sean)"
    say "  output:  $GLOG"
  fi
fi

say ""; say "---- 4. the matrix rows that touch the change ----"
R0=$(date +%s)
RRC=0
if [ "$ROWS" = "none" ]; then
  say "(none named - say which rows touch the change and they run here)"
else
  V2="/Users/SeanD/echojay-vst/$TREE/EchoJay_artefacts/Release/AU/EchoJay V2.component"
  LK="/Users/SeanD/echojay-vst/$TREE/EchoJayLink_artefacts/Release/AU/EchoJay Link.component"
  V3="/Users/SeanD/echojay-vst/$TREE/EchoJay_artefacts/Release/VST3/EchoJay V2.vst3"
  EJ_RIG_ONLY="$ROWS" EJ_RIG_LIVE= EJ_RIG_TOKEN= \
    bash tools/host_rig/build_and_run.sh "$V2" "$LK" "" "$V3" > "$S/quick_rows.log" 2>&1
  RRC=$?
  grep -E "FILTERED RUN|  (SCEN|ROW) " "$S/quick_rows.log" | sed -e 's/.*host_rig\[[0-9:]*\] //' \
    | cut -c1-200 | tee -a "$LOG"
  grep -E "  (SCEN|ROW) .*FAIL" -A 1 "$S/quick_rows.log" | sed -e 's/.*host_rig\[[0-9:]*\] //' \
    | grep -vE "^--|evidence" | cut -c1-230 | tee -a "$LOG"
fi
say "rows in $(mins $(( $(date +%s) - R0 )))  (exit $RRC)"

EL=$(( $(date +%s) - T0 ))
say ""
say "######## QUICK DONE in $(mins $EL) — he can test now; the guards finish on their own"
say "This is NOT a gate: no forced archive delete, no three trees, no full matrix,"
say "no live pass, no LTO and one architecture. The FULL gate runs overnight and"
say "before anything goes to beta, and it builds the real ship build."
say "log: $LOG"
say "revert: bash tools/install_local.sh build-guards   (the last LTO tree's bundles)"
exit $RRC
