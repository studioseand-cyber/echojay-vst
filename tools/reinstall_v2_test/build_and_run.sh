#!/bin/zsh
#
# reinstall_v2_test — the version-bump assertions in tools/reinstall-v2.sh.
#
# NOTHING TO BUILD. Unlike the other suites here this is pure shell, which is
# why the pre-commit gate runs it BEFORE its unbuilt-tree skip: it costs
# milliseconds and it is exactly the sort of thing that must not go dark
# because a C++ library happens to be missing.
#
# It EXTRACTS the real functions from tools/reinstall-v2.sh and evaluates
# them, rather than restating the logic. A copy would drift, and the drift
# would be invisible because this file's own tests would keep passing. If the
# functions are renamed or removed, the extraction fails loudly as not-found
# instead of silently testing nothing — which is the failure this suite was
# written about in the first place.
#
# What it covers: sed reporting SUCCESS for a substitution that matched
# nothing. build-installer.sh and package-dmg.sh sat thirteen versions behind
# CMakeLists.txt on exactly that, unnoticed, because every run replaced
# nothing and exited 0.
set -u

# Resolved from THIS FILE'S OWN LOCATION, never with git.
#
# `git rev-parse --show-toplevel` is wrong here and wrong in a way that only
# shows up under the gate: git EXPORTS GIT_DIR when it runs a hook, and with
# GIT_DIR set, `git -C <dir> rev-parse --show-toplevel` returns <dir> itself
# rather than searching upward for the real root. So it resolves correctly
# when you run this by hand and resolves to the test's own directory when the
# pre-commit gate runs it — which is how it was caught, on its first gated
# commit. The sibling suites take paths from $0 for the same reason.
HERE="${0:A:h}"                    # tools/reinstall_v2_test
SRC="${HERE:h}/reinstall-v2.sh"    # tools/reinstall-v2.sh

[ -f "$SRC" ] || { echo "FAIL: $SRC not found"; exit 1; }

# By PATTERN, not by line number: an edit above these functions must not
# silently shift the window onto the wrong text. Prints from sed_verify()'s
# opening line to the second closing brace at column 0, which is the end of
# bumpfiles().
FN=$(awk '/^sed_verify\(\) \{/ {on=1}
          on {print}
          on && /^\}/ {n++; if (n==2) exit}' "$SRC")

case "$FN" in
  *"sed_verify()"*) ;;
  *) echo "FAIL: sed_verify() not found in $SRC (renamed or removed?)"; exit 1 ;;
esac
case "$FN" in
  *"bumpfiles()"*) ;;
  *) echo "FAIL: bumpfiles() not found in $SRC (renamed or removed?)"; exit 1 ;;
esac
eval "$FN"

# stamp_guard() sits earlier in the file, so it gets its own window: from its
# opening line to the first closing brace at column 0.
GFN=$(awk '/^stamp_guard\(\) \{/ {on=1}
           on {print}
           on && /^\}/ {exit}' "$SRC")
case "$GFN" in
  *"stamp_guard()"*) ;;
  *) echo "FAIL: stamp_guard() not found in $SRC (renamed or removed?)"; exit 1 ;;
esac
eval "$GFN"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
REPO="$TMP"; CML="$TMP/CMakeLists.txt"   # the names the extracted functions read

pass=0; fail=0
ck() { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1));
       else echo "  FAIL $1 (got '$2', want '$3')"; fail=$((fail+1)); fi }

mkfixture() {  # $1 = version carried by the two installer scripts
  print 'project(EchoJay VERSION 2.25.10)' > "$CML"
  print "VERSION=\"$1\"" > "$REPO/build-installer.sh"
  print "VERSION=\"$1\"" > "$REPO/package-dmg.sh"
}

# 1. All three in sync: the bump succeeds and every file moves.
mkfixture "2.25.10"
bumpfiles "2.25.10" "2.25.11" >/dev/null; ck "in-sync bump returns 0" "$?" "0"
ck "CMakeLists moved" "$(grep -c 'VERSION 2.25.11' "$CML")" "1"
ck "installer moved"  "$(grep -c '2.25.11' "$REPO/build-installer.sh")" "1"
ck "dmg moved"        "$(grep -c '2.25.11' "$REPO/package-dmg.sh")" "1"

# 2. THE REGRESSION ITSELF: a file left behind must fail, and say which.
mkfixture "2.24.2"
OUT=$(bumpfiles "2.25.10" "2.25.11"); rc=$?
ck "drifted file returns non-zero" "$rc" "1"
ck "names the installer"  "$(print -r -- "$OUT" | grep -c 'STALE build-installer.sh')" "1"
ck "names the dmg"        "$(print -r -- "$OUT" | grep -c 'STALE package-dmg.sh')" "1"
ck "CMakeLists still moved (partial bump is real)" \
   "$(grep -c 'VERSION 2.25.11' "$CML")" "1"

# 3. A MISSING file is a skip, not a failure — older branches still build.
mkfixture "2.25.10"; rm "$REPO/package-dmg.sh"
OUT=$(bumpfiles "2.25.10" "2.25.11"); rc=$?
ck "missing file returns 0" "$rc" "0"
ck "and is reported as SKIP" "$(print -r -- "$OUT" | grep -c 'SKIP  package-dmg.sh')" "1"

# 4. The revert direction works, or the abort path cannot unwind a partial
#    bump and would leave the tree on a version that was never built.
mkfixture "2.25.10"
bumpfiles "2.25.10" "2.25.11" >/dev/null
bumpfiles "2.25.11" "2.25.10" >/dev/null; ck "revert returns 0" "$?" "0"
ck "CMakeLists back" "$(grep -c 'VERSION 2.25.10' "$CML")" "1"

# 5. THE STAMP GUARD, both directions — a refusal nobody has watched happen
#    is not a guard, and a guard that also refuses the primary worktree is a
#    dead script. Real worktrees, not a mock: the property under test IS git's
#    git-dir/common-dir distinction.
#
#    UNSET THE HOOK'S GIT ENV FIRST. These are the only git commands this
#    harness runs, and under the pre-commit gate git has exported GIT_DIR and
#    GIT_INDEX_FILE — with those set, the fixture's init/commit/worktree-add
#    operate on the REPO BEING COMMITTED instead of the fixture. On this
#    suite's first gated run (15 Aug 2026) that committed the gate's staged
#    index onto feat/plugin-dashboard as "fixture" and registered a ghost
#    worktree. Same trap the header documents for $0 resolution: under a
#    hook, nothing may reach git through inherited environment.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
GT="$TMP/guard"
git init -q "$GT/prim"
git -C "$GT/prim" -c user.email=t@t.t -c user.name=t commit -q --allow-empty -m fixture
git -C "$GT/prim" worktree add -q -b guard-feature "$GT/wt" >/dev/null 2>&1

OUT=$(stamp_guard "$GT/wt"); rc=$?
ck "guard refuses a linked worktree" "$rc" "1"
ck "and names the worktree" "$(print -r -- "$OUT" | grep -c "REFUSED: $GT/wt")" "1"
ck "and names its branch"   "$(print -r -- "$OUT" | grep -c "guard-feature")" "1"
stamp_guard "$GT/prim" >/dev/null
ck "guard permits the primary worktree" "$?" "0"

# The worktree registration lives under prim/.git and dies with $TMP's rm -rf;
# nothing to prune.

echo "reinstall_v2_test: $pass passed, $fail failed"
exit $(( fail > 0 ? 1 : 0 ))
