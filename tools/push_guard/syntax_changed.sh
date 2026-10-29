#!/bin/bash
# =============================================================================================
#  syntax_changed.sh - COMPILE EVERY SOURCE FILE A PUSH WOULD CARRY, BEFORE IT LEAVES
#  (Sean's ruling, 29 Oct 2026, after Kathy's build died on feat/08c at 43519fd8)
# =============================================================================================
#  What happened: 92487210 went in with two unescaped quotes in a log line. It was committed
#  during a no-builds window (Sean was testing in Logic), the fix rode into the NEXT commit as an
#  unrelated rider, and the build afterwards compiled a WORKING TREE that already had the fix - so
#  nothing ever compiled that commit AS COMMITTED. Kathy checked out 43519fd8 and lost her morning.
#
#  His instruction: "add whatever makes a commit that doesn't compile impossible to push".
#
#  ---- IT USES THE BUILD'S OWN FLAGS, WHICH COST THIS GUARD A REWRITE ------------------------
#
#  The first cut passed a hand-written flag set and REFUSED three files that were perfectly fine:
#  without the plugin's defines, `JucePlugin_Name` reads as "use of undeclared identifier" and a
#  binary-data header reads as "file not found" - the same words a real fault produces. A guard
#  that cries wolf is turned off within a day, so the flags come from compile_commands.json: the
#  exact command CMake uses for that file, with -fsyntax-only and the committed copy swapped in.
#  A file CMake does not know about is reported as NOT CHECKED, by name, rather than guessed at.
#
#  AS COMMITTED, never from the working tree - the working tree is what hid the fault.
# =============================================================================================
set -u
RANGE="${1:-}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1

if [[ "${EJ_PUSH_GUARD_OFF:-0}" == "1" ]]; then
  echo "push-guard: OFF by EJ_PUSH_GUARD_OFF=1 - nothing was compiled, and that is on the pusher."
  exit 0
fi
[[ -z "$RANGE" ]] && { echo "push-guard: no range given, nothing to check"; exit 0; }

# EVERY database, not the first: the plugin targets are in build/ and some harnesses are in
# build-release/ or build-guards/ only, and a file nobody knows is reported as unverified by name.
CC=""
for d in build build-quick build-guards build-release; do
  [[ -f "$ROOT/$d/compile_commands.json" ]] && CC="$CC $ROOT/$d/compile_commands.json"
done
CC="${CC# }"
if [[ -z "$CC" ]]; then
  echo "push-guard: CANNOT COMPILE - no compile_commands.json in build/, build-quick/,"
  echo "push-guard: build-guards/ or build-release/. Configure a build once, or push with"
  echo "push-guard: EJ_PUSH_GUARD_OFF=1 and say in the message that nothing was compiled."
  exit 1
fi

TIP=$(git rev-parse "${RANGE##*..}" 2>/dev/null) || TIP=$(git rev-parse HEAD)
FILES=$(git diff --name-only --diff-filter=ACMR "$RANGE" -- 'Source/*.cpp' 'Source/*.mm' 'tools/*.cpp' 'tools/*.mm' 2>/dev/null | sort -u)
[[ -z "$FILES" ]] && { echo "push-guard: no C++ source files in $RANGE"; exit 0; }

python3 -I "$ROOT/tools/push_guard/syntax_changed.py" "$CC" "$TIP" $FILES
