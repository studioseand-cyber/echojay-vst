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
# ---- A PATH WITH A SPACE IN IT IS STILL A PATH (Kathy, 30 Oct 2026) ----------------------
#
# This built a SPACE-JOINED string of database paths and the python split it on spaces. Kathy's
# checkout lives under "ECHOJAY FILES", so every path came apart, not one database could be read,
# all thirteen of her files were reported NOT CHECKED - and the push PASSED, "unverified". A guard
# that cannot read its own inputs must not wave the push through; that is worse than no guard,
# because the line it prints looks like diligence.
#
# So the databases travel as an ARRAY, each one its own argv entry after a --dbs marker, and the
# files after a --files marker. No quoting rule to remember and no splitting anywhere.
CC_DBS=()
for d in build build-quick build-guards build-release; do
  [[ -f "$ROOT/$d/compile_commands.json" ]] && CC_DBS+=("$ROOT/$d/compile_commands.json")
done
if [[ ${#CC_DBS[@]} -eq 0 ]]; then
  echo "push-guard: CANNOT COMPILE - no compile_commands.json in build/, build-quick/,"
  echo "push-guard: build-guards/ or build-release/. Configure a build once, or push with"
  echo "push-guard: EJ_PUSH_GUARD_OFF=1 and say in the message that nothing was compiled."
  exit 1
fi

# ---- AND THE RANGE MUST NOT SPAN A REBASE (Kathy, 30 Oct 2026) ---------------------------
#
# Her second fault: after she rebased, the pre-push range was remote_sha..local_sha, and the remote
# sha was no longer an ancestor of hers - so the diff spanned the rebase and swept in MY files, and
# the guard reported her push as touching work she had never seen. The honest comparison point is
# where the two histories actually part.
RANGE_BASE="${RANGE%%..*}"
RANGE_TIP="${RANGE##*..}"
if [[ -n "$RANGE_BASE" && "$RANGE_BASE" != "$RANGE" ]] \
   && ! git merge-base --is-ancestor "$RANGE_BASE" "$RANGE_TIP" 2>/dev/null; then
  if MB=$(git merge-base "$RANGE_BASE" "$RANGE_TIP" 2>/dev/null) && [[ -n "$MB" ]]; then
    echo "push-guard: ${RANGE_BASE:0:8} is not an ancestor of ${RANGE_TIP:0:8} (a rebase, or a"
    echo "push-guard: forced update) - comparing from the merge-base ${MB:0:8} instead, so this"
    echo "push-guard: push is judged on its own files and not on everything the rebase moved."
    RANGE="$MB..$RANGE_TIP"
  fi
fi

TIP=$(git rev-parse "$RANGE_TIP" 2>/dev/null) || TIP=$(git rev-parse HEAD)
FILES=$(git diff --name-only --diff-filter=ACMR "$RANGE" -- 'Source/*.cpp' 'Source/*.mm' 'tools/*.cpp' 'tools/*.mm' 2>/dev/null | sort -u)
[[ -z "$FILES" ]] && { echo "push-guard: no C++ source files in $RANGE"; exit 0; }

# shellcheck disable=SC2086  # FILES is a newline list of paths with no spaces (git's own output)
python3 -I "$ROOT/tools/push_guard/syntax_changed.py" \
        --tip "$TIP" --dbs "${CC_DBS[@]}" --files $FILES
