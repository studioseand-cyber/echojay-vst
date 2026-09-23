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
"$BIN" "$@"; RC=$?
echo "exit code: $RC  (0 == GREEN, nonzero == RED)"
if [ $SCRIBBLE -eq 1 ]; then
  echo "SCRIBBLE LEG: running again with MallocScribble=1 MallocPreScribble=1 MallocGuardEdges=1 ..."
  SLOG="$ISO/scribble.out"
  MallocScribble=1 MallocPreScribble=1 MallocGuardEdges=1 "$BIN" "$@" > "$SLOG" 2>&1; SRC=$?
  # On a failing scribble leg the reason has to be READABLE - a bare exit code is not evidence.
  if [ $SRC -ne 0 ]; then echo "---- scribble leg output (tail) ----"; tail -40 "$SLOG"; echo "---- end scribble leg output ----"; fi
  echo "scribble leg exit code: $SRC  (0 == GREEN, nonzero == RED)"
  if [ $RC -eq 0 ] && [ $SRC -eq 0 ]; then echo "BOTH LEGS: GREEN"; else echo "BOTH LEGS: RED (plain $RC, scribble $SRC)"; fi
  [ $RC -eq 0 ] && [ $SRC -eq 0 ] || RC=1
fi
rm -rf "$ISO" 2>/dev/null
exit $RC
