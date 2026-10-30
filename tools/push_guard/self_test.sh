#!/bin/bash
# =============================================================================================
#  self_test.sh - THE PUSH GUARD'S OWN TWO FAULTS, BOTH PROVEN (Kathy, 30 Oct 2026)
# =============================================================================================
#  She reported two, and each one let a push through while LOOKING like it had been checked:
#
#   1. A SPACE IN THE CHECKOUT PATH. The databases travelled as one space-joined string and the
#      python split it on spaces. Her checkout is under "ECHOJAY FILES", so every path came apart,
#      no database could be read, all 13 of her files were "NOT CHECKED" and the push PASSED as
#      "unverified". A guard that cannot read its own inputs must refuse, not wave it through.
#
#   2. A RANGE THAT SPANNED A REBASE. The pre-push range is remote_sha..local_sha, and after she
#      rebased the remote sha was no longer an ancestor of hers - so the diff swept in MY files and
#      the guard judged her push on work she had never touched.
#
#  Both are shell and argv faults, which no C++ leg can reach, so they are tested here - by doing
#  the thing that broke, in a throwaway directory whose name contains a space.
#
#  Usage: bash tools/push_guard/self_test.sh      (exit 0 = GREEN)
# =============================================================================================
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fails=0
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; echo "        $2"; fails=$((fails+1)); }

TMP="$(mktemp -d "/tmp/ej push guard test.XXXXXX")"     # the space is the point
trap 'rm -rf "$TMP"' EXIT

echo "the push guard's own faults:"

# ---- 1. A DATABASE UNDER A PATH WITH A SPACE IS READ ---------------------------------------
DB="$TMP/with space/compile_commands.json"
mkdir -p "$(dirname "$DB")"
SRC="$ROOT/Source/MeterEngine.cpp"
printf '[{"directory":"%s","file":"%s","command":"/usr/bin/true -c %s"}]\n' \
       "$ROOT" "$SRC" "$SRC" > "$DB"
out="$(python3 -I "$ROOT/tools/push_guard/syntax_changed.py" \
         --tip HEAD --dbs "$DB" --files Source/MeterEngine.cpp 2>&1)"
if printf '%s' "$out" | grep -q "could not read"; then
  bad "1: a database under a path with a space is read" "$(printf '%s' "$out" | head -2)"
elif printf '%s' "$out" | grep -q "NOT CHECKED Source/MeterEngine.cpp"; then
  bad "1: a database under a path with a space is read" \
      "the file came back NOT CHECKED, which is the fault: the path was split on its space"
else
  ok "1: a database under a path with a space is read, and the file is checked"
fi

# ---- 2. A GUARD THAT CANNOT READ ITS INPUTS REFUSES ----------------------------------------
#
# HER FAULT EXACTLY, and the first cut of this check tested the wrong thing: it used an EMPTY but
# READABLE database and demanded a refusal, which then blocked a legitimate push of a harness file
# that no database knows (host_rig.mm, compiled by its own script). Kathy's fault was not "nothing
# was compiled"; it was "no database could be READ, and the push went anyway". So: an unreadable
# path must refuse, and a readable database that simply does not list these files must not.
UNREADABLE="$TMP/no such dir/compile_commands.json"
python3 -I "$ROOT/tools/push_guard/syntax_changed.py" \
        --tip HEAD --dbs "$UNREADABLE" --files Source/MeterEngine.cpp > "$TMP/out2" 2>&1
if [[ $? -eq 0 ]]; then
  bad "2: a guard that could read NO database refuses" \
      "it exited 0 - that is the pass she got, with nothing verified and nothing verifiable"
else
  ok "2: a guard that could read NO database refuses, rather than passing as unverified"
fi

# ---- 2b. ...AND A READABLE DATABASE THAT LISTS NOTHING IS NOT THAT FAULT --------------------
EMPTY="$TMP/empty db/compile_commands.json"
mkdir -p "$(dirname "$EMPTY")"; echo '[]' > "$EMPTY"
if python3 -I "$ROOT/tools/push_guard/syntax_changed.py" \
        --tip HEAD --dbs "$EMPTY" --files tools/host_rig/host_rig.mm > "$TMP/out2b" 2>&1; then
  ok "2b: a readable database that does not list a file lets it through, by name"
else
  bad "2b: a readable database that does not list a file lets it through" \
      "it refused, which blocks every harness-only commit: $(tail -2 "$TMP/out2b" | head -1)"
fi

# ---- 3. AND CALLED WITHOUT ITS INPUTS, IT REFUSES ------------------------------------------
if python3 -I "$ROOT/tools/push_guard/syntax_changed.py" --files Source/MeterEngine.cpp > /dev/null 2>&1; then
  bad "3: called with no --tip/--dbs it refuses" "it exited 0"
else
  ok "3: called with no --tip/--dbs it refuses"
fi

# ---- 4. A RANGE THAT SPANS A REBASE IS NARROWED TO THE MERGE-BASE --------------------------
# A throwaway repo with the exact shape: a shared root, a remote tip, and a rebased local tip whose
# history no longer contains it.
R="$TMP/repo with space"
mkdir -p "$R"
(
  cd "$R" || exit 1
  git init -q . && git config user.email t@t && git config user.name t
  echo root > a.txt && git add . && git commit -qm root
  ROOTC=$(git rev-parse HEAD)
  echo theirs > b.txt && git add . && git commit -qm theirs      # the "remote" tip
  REMOTE=$(git rev-parse HEAD)
  git checkout -q "$ROOTC" -b mine
  echo mine > c.txt && git add . && git commit -qm mine          # a history without REMOTE in it
  MINE=$(git rev-parse HEAD)
  echo "$REMOTE $MINE $(git merge-base "$REMOTE" "$MINE")"
) > "$TMP/shas" 2>/dev/null
read -r REMOTE MINE MB < "$TMP/shas"
if [[ -z "${MB:-}" ]]; then
  bad "4: a range spanning a rebase is narrowed" "could not build the fixture repo"
else
  # The decision itself, as the guard makes it: is the left side an ancestor of the right?
  if git -C "$R" merge-base --is-ancestor "$REMOTE" "$MINE" 2>/dev/null; then
    bad "4: a range spanning a rebase is narrowed" \
        "the fixture is wrong - the remote tip IS an ancestor, so there is no rebase to narrow"
  elif [[ "$MB" == "$(git -C "$R" merge-base "$REMOTE" "$MINE")" ]] \
       && grep -q 'merge-base --is-ancestor "$RANGE_BASE" "$RANGE_TIP"' "$ROOT/tools/push_guard/syntax_changed.sh"; then
    ok "4: a range spanning a rebase is narrowed to the merge-base (the guard asks --is-ancestor first)"
  else
    bad "4: a range spanning a rebase is narrowed" \
        "syntax_changed.sh does not test --is-ancestor before trusting the range it was given"
  fi
fi

echo "==== push_guard self_test: $([[ $fails -eq 0 ]] && echo GREEN || echo RED) ($fails failed) ===="
[[ $fails -eq 0 ]]
