#!/bin/bash
# EchoJay guard runner (21q, 23 Sep 2026) — ONE runner for every guard in the CMake test tree.
#
# It does exactly what the per-guard build_and_run.sh scripts did at RUN time, and nothing they did at BUILD time
# (CMake owns building now):
#   • a private HOME, ECHOJAY_STATE_HOME, EJ_STATE_TEST_HOME and TMPDIR per run, so no guard can see the user's live
#     state or another guard's - the isolation rule, unchanged;
#   • the SCRIBBLE leg for the guards whose script ran one (MallocScribble/MallocPreScribble/MallocGuardEdges), and
#     BOTH legs must pass, exactly as harness_build.py required;
#   • the guard's own stdout, verbatim, so the summary reads the same lines it always did.
# Usage: run_guard.sh <binary> [--scribble] [-- <extra args...>]
set -u
BIN="$1"; shift
SCRIBBLE=0
while [ $# -gt 0 ]; do case "$1" in --scribble) SCRIBBLE=1; shift;; --) shift; break;; *) break;; esac; done
# ISOLATION IS A PRECONDITION, NOT A BEST EFFORT. The base directory is handed down by CMake
# (EJ_GUARD_HOME_BASE, inside the build tree) and NOT taken from TMPDIR: under ctest, TMPDIR pointed at the
# sandboxed per-user /var/folders/.../T, mkdtemp there returned "Permission denied", and the old line let the
# guard run on with an EMPTY HOME - which is how two guards reported RED against a path of "/Library/EchoJay".
# A guard that cannot be isolated does not get a verdict: exit 2 means the runner refused to run, never a failure.
BASE="${EJ_GUARD_HOME_BASE:-/tmp}"
mkdir -p "$BASE" 2>/dev/null
ISO="$(mktemp -d "$BASE/ejguard-$(basename "$BIN").XXXXXX" 2>&1)" || { echo "RUNNER REFUSED: no private HOME under $BASE ($ISO)"; exit 2; }
if [ -z "$ISO" ] || [ ! -d "$ISO" ]; then echo "RUNNER REFUSED: no private HOME under $BASE"; exit 2; fi
export HOME="$ISO" ECHOJAY_STATE_HOME="$ISO" EJ_STATE_TEST_HOME="$ISO" TMPDIR="$ISO"
mkdir -p "$ISO/Library/Application Support" "$ISO/Documents"

# ---- THE LIVE-STATE SEAL (26 Sep 2026 ruling) -------------------------------------------------------------
# ISOLATION IS ENFORCED, THEN ASSERTED. A private HOME is not enough: juce::userDocumentsDirectory and
# userApplicationDataDirectory resolve from the password database and IGNORE $HOME, so a harness reaching them
# lands in the LIVE folders no matter what this script exports. That is not hypothetical - the dev-mode chat-body
# dump did exactly that, and its rolling history evicted the dumps from real sessions before anyone noticed.
#
# TWO MECHANISMS, and the order matters:
#
#  1. THE SANDBOX, which is the one that attributes. Every guard runs under sandbox-exec with file-write DENIED to
#     the three live folders (reads stay allowed - a guard may legitimately look at them). A harness that tries to
#     write gets "Operation not permitted" at the syscall, in ITS process, whatever else is running on the Mac.
#  2. THE FINGERPRINT, as ruled: size+mtime of every file two levels deep in each live folder, before and after.
#     It is kept because the sandbox could be unavailable, but it CANNOT ATTRIBUTE: the live plugin in a running
#     host rewrites racklock-*.json, lease-*.json and the rolling log every few seconds, so while a host is live a
#     difference is not evidence about this guard (observed 26 Sep 2026, Pro Tools open, every guard "guilty").
#     So a difference is a FAILURE when no EchoJay host is running, and a named, printed inconclusive otherwise.
REAL_HOME="$(/usr/bin/id -P "$(/usr/bin/id -un)" 2>/dev/null | /usr/bin/cut -d: -f9)"
[ -n "$REAL_HOME" ] && [ -d "$REAL_HOME" ] || REAL_HOME="/Users/$(/usr/bin/id -un)"
SEAL_TARGETS=("$REAL_HOME/Documents/EchoJay" "$REAL_HOME/Library/EchoJay" "$REAL_HOME/Library/Application Support/EchoJay" "$REAL_HOME/Library/Logs/EchoJay" "$REAL_HOME/.echojay")
seal () {  # one line per file/dir: <size> <mtime> <path>
  local d
  for d in "${SEAL_TARGETS[@]}"; do
    if [ -d "$d" ]; then
      /usr/bin/find "$d" -maxdepth 2 \( -type f -o -type d \) -exec /usr/bin/stat -f '%z %m %N' {} \; 2>/dev/null | LC_ALL=C sort
    else
      echo "ABSENT $d"
    fi
  done
}
SEAL_BEFORE="$ISO/.live_seal_before"; SEAL_AFTER="$ISO/.live_seal_after"
seal > "$SEAL_BEFORE" 2>/dev/null

# The sandbox profile: allow everything, deny writes into the three live folders.
PROFILE="$ISO/live_state.sb"
{
  echo '(version 1)'
  echo '(allow default)'
  echo '(deny file-write*'
  for d in "${SEAL_TARGETS[@]}"; do echo "  (subpath \"$d\")"; done
  echo ')'
} > "$PROFILE"
SEALED=1
if ! command -v sandbox-exec >/dev/null 2>&1; then
  SEALED=0
  echo "LIVE-STATE SEAL: sandbox-exec is not available, so this run is NOT write-sealed (the fingerprint below is"
  echo "                 the only check, and it cannot attribute a change while an EchoJay host is running)."
fi
# BOTH LEGS ARE SEALED, and the reason that sentence is here at all: the first cut of this runner ran the scribble
# leg UNSEALED, because one sealed run of loudness_loop_guard trapped and one unsealed run was green. Four runs
# each says otherwise - sealed 1/4 green, unsealed 2/4 green, every failure AFTER the last assertion (149-150 ok)
# with the signal varying (134/138/139). That is loudness_loop_guard's own flaky teardown, not the sandbox, and a
# decision taken on one observation of each was a decision about scheduling noise. The seal stays on both.
run_guard () {
  if [ $SEALED -eq 1 ]; then sandbox-exec -f "$PROFILE" "$BIN" "$@"; else "$BIN" "$@"; fi
}

run_guard "$@"; RC=$?
echo "exit code: $RC  (0 == GREEN, nonzero == RED)"

seal > "$SEAL_AFTER" 2>/dev/null
if ! /usr/bin/cmp -s "$SEAL_BEFORE" "$SEAL_AFTER"; then
  HOSTPIDS="$(/usr/bin/pgrep -f "Pro Tools|Logic Pro|AUHostingService|AAXHostService|AAEHostService|EchoJay" 2>/dev/null | tr '\n' ' ')"
  echo "LIVE-STATE FINGERPRINT CHANGED in $REAL_HOME (Documents/EchoJay, Library/EchoJay, Library/Application Support/EchoJay):"
  /usr/bin/diff "$SEAL_BEFORE" "$SEAL_AFTER" | /usr/bin/head -20 | /usr/bin/sed 's/^/  /'
  if [ -n "$HOSTPIDS" ]; then
    echo "  INCONCLUSIVE, NOT ATTRIBUTED: a live EchoJay host is running (pid(s) $HOSTPIDS) and rewrites its lock,"
    echo "  lease and rolling log every few seconds. The write seal above is what covers this guard; the"
    echo "  fingerprint is reported, not charged."
  else
    echo "  VIOLATION: nothing else was running, so this guard changed the user's live state. Its result is void."
    RC=1
  fi
fi
if [ $SCRIBBLE -eq 1 ]; then
  echo "SCRIBBLE LEG: running again with MallocScribble=1 MallocPreScribble=1 MallocGuardEdges=1 ..."
  SLOG="$ISO/scribble.out"
  # THE SCRIBBLE LEG GETS ITS OWN STATE ROOT (2 Oct 2026). It did not, and that cost a night.
  #
  # Both legs ran with the one $ISO as HOME/ECHOJAY_STATE_HOME, so the scribble leg was not a second
  # INDEPENDENT run of the guard - it was the guard run a second time ON TOP OF the first run's state. Guards
  # legitimately persist: level_loop_guard case (6a) calls storeParamMaps to mark Apple's AUDelay
  # category=compressor, and that lands in $ISO/Library/EchoJay/param_maps.json. On the second leg a fresh
  # ChainHost loads it back, so (6a)'s own precondition - "a delay is not a dynamics slot by its OWN category" -
  # read 1 and failed. comp_profile_guard's (2a), "with no profile published, the slot has none", failed for the
  # same reason.
  #
  # It looked like a memory bug for hours, because the only leg that failed was the one with malloc hardening on.
  # It is not: TWO PLAIN RUNS in one state root reproduce it exactly, with no MallocScribble anywhere -
  #     run 1: exit=0  (6a) ok   [0]
  #     run 2: exit=1  (6a) FAIL [1]
  # Isolation is a precondition for evidence, and that applies BETWEEN THE TWO LEGS of one guard, not just
  # between a guard and the user's live state. A second leg sharing the first's state root tests neither the
  # product nor the allocator; it tests whether the guard happens to be idempotent.
  SISO="$(mktemp -d "$BASE/ejguard-$(basename "$BIN")-scrib.XXXXXX" 2>&1)" || { echo "  FAIL  no private HOME for the scribble leg under $BASE ($SISO)"; SISO=""; }
  if [ -z "$SISO" ] || [ ! -d "$SISO" ]; then
    echo "  FAIL  the scribble leg has no private state root, so its result would not be evidence - REFUSED."
    SRC=1
  else
    mkdir -p "$SISO/Library/Application Support" "$SISO/Documents"
    HOME="$SISO" ECHOJAY_STATE_HOME="$SISO" EJ_STATE_TEST_HOME="$SISO" TMPDIR="$SISO"       MallocScribble=1 MallocPreScribble=1 MallocGuardEdges=1 run_guard "$@" > "$SLOG" 2>&1; SRC=$?
    rm -rf "$SISO" 2>/dev/null
  fi
  # On a failing scribble leg the reason has to be READABLE - a bare exit code is not evidence.
  # THE TAIL IS NOT THE REASON (1 Oct 2026). level_loop_guard failed a scribble leg with "RED (1 assertion(s)
  # failed)" and the 40-line tail was forty `ok` lines, because the failure was earlier in the run - so the log
  # proved only that SOMETHING failed. Every FAIL line is printed first, by name, and the tail after it for context.
  if [ $SRC -ne 0 ]; then
    echo "---- scribble leg FAILURES ----"
    grep -E "^[[:space:]]*FAIL" "$SLOG" || echo "(no FAIL line: the leg died without reporting one - see the tail)"
    echo "---- scribble leg output (tail) ----"; tail -40 "$SLOG"; echo "---- end scribble leg output ----"
  fi
  echo "scribble leg exit code: $SRC  (0 == GREEN, nonzero == RED)"
  if [ $RC -eq 0 ] && [ $SRC -eq 0 ]; then echo "BOTH LEGS: GREEN"; else echo "BOTH LEGS: RED (plain $RC, scribble $SRC)"; fi
  [ $RC -eq 0 ] && [ $SRC -eq 0 ] || RC=1
fi
rm -rf "$ISO" 2>/dev/null
exit $RC
