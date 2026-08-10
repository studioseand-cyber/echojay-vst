#!/bin/zsh
# EchoJay dev reinstall — kill AU host, bump version, rebuild, install (atomic).
#
# THE TRACKED COPY. ~/reinstall-v2.sh is a thin shim that resolves the repo
# with `git rev-parse --show-toplevel` and execs this file, so you keep
# typing `~/reinstall-v2.sh` and get the copy belonging to the worktree you
# are standing in — per branch, under review, and bisectable.
#
# It lived in $HOME until 3 Aug 2026 and that was the wrong place. Two edits
# in one afternoon (a -j default, then an abort-on-stale-bump) went live for
# five worktrees the instant they were saved, with no history and no review,
# and the second one can abort a build that previously succeeded. Editing
# THIS file changes only the branch you edit it on.
#
# Tests: tools/reinstall_v2_test/build_and_run.sh, run by the pre-commit
# gate. It extracts the real functions from this file rather than copying
# them, so an edit here is either covered or fails loudly as not-found.
# Version convention: v2.MM.PP, major pinned at 2. STORED version is always
# numeric/unpadded (AU version code, Info.plist, pkgbuild --version, the
# update check and the API appVersion all parse it); two-digit padding is
# DISPLAY-ONLY (this script's output, installer/dmg filenames, plugin UI).

# 0. BUILD LOCK, before anything else including the killall.
#
#    The install destination is SHARED across worktrees: every branch installs
#    to ~/Library/Audio/Plug-Ins/Components under the same bundle name, so two
#    sessions building at once means whoever finishes last wins and the version
#    on screen stops being evidence of anything. That has already cost an
#    afternoon once (see the binary-verification note in
#    CHAIN_AI_BUILD_SPEC.md).
#
#    mkdir is ATOMIC: it either creates the directory or fails, with no window
#    between checking and acting. A `[ -d ... ]` test followed by a mkdir would
#    be a check-then-act race and would let both sessions through.
#
#    The trap is armed ONLY after a successful acquire, so a session that finds
#    the lock held cannot remove somebody else's lock on its way out.
LOCK=~/.echojay-build.lock
if ! mkdir "$LOCK" 2>/dev/null; then
  echo "ABORT: another session holds the build lock ($LOCK)."
  echo "  Held since: $(stat -f '%Sm' "$LOCK" 2>/dev/null)"
  echo "  Nothing was built and nothing was installed."
  echo "  If you are certain no build is running, remove it with: rmdir $LOCK"
  exit 1
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT INT TERM
echo "Build lock acquired."

# 1. Drop the running plugin image FIRST so a cached binary can't mask the
#    install (this runs even if the build later fails — that's fine, the old
#    on-disk component stays in place and simply reloads).
killall AudioComponentRegistrar AUHostingServiceXPC_arrow 2>/dev/null || true

REPO="$(git rev-parse --show-toplevel 2>/dev/null)"
if [ -z "$REPO" ]; then
  echo "Not inside a git repo. cd into the tree you want to build first."
  exit 1
fi
BUILD="$REPO/build"
DEST=~/Library/Audio/Plug-Ins/Components
CML="$REPO/CMakeLists.txt"

# 2. Bump patch (+rollover) on the single stored source of truth.
CUR=$(sed -n 's/^project(EchoJay VERSION 2\.\([0-9]*\)\.\([0-9]*\))$/\1 \2/p' "$CML")
MINOR=${CUR%% *}; PATCH=${CUR##* }
[ -z "$MINOR" ] && { echo "ABORT: could not parse project(EchoJay VERSION 2.x.y) in CMakeLists.txt"; exit 1; }
OLD="2.$MINOR.$PATCH"
PATCH=$((PATCH + 1))
if [ $PATCH -gt 99 ]; then PATCH=0; MINOR=$((MINOR + 1)); fi
if [ $MINOR -gt 99 ]; then echo "ABORT: minor would exceed 99 (v2.$MINOR) — format cap reached, bump manually."; exit 1; fi
NEW="2.$MINOR.$PATCH"                                   # stored, numeric
DISPLAY=$(printf "v2.%02d.%02d" $MINOR $PATCH)          # human-facing only

# `sed -i` REPORTS SUCCESS WHEN IT REPLACES NOTHING. That is not a detail:
# build-installer.sh and package-dmg.sh sat at 2.24.2 while CMakeLists.txt
# climbed to 2.25.10 — THIRTEEN versions — because every bump searched those
# files for a string they no longer contained, replaced nothing, and exited 0.
# Nothing was broken, nothing was logged, and nothing could ever have pointed
# at the cause. Only a release would have surfaced it, by packaging a 2.25.x
# plugin as 2.24.2.
#
# So each substitution is now VERIFIED by comparing the file before and after,
# which is the only thing sed's exit status will not tell you.
#
# A MISSING file is a loud skip, not a failure — same discipline as the
# self-tests below, so an older branch without one of these still builds. A
# file that is PRESENT and did not change is the drift itself, and aborts.
sed_verify() {  # $1 = file, $2 = sed expression;  non-zero if nothing changed
  local file="$1" expr="$2" before after
  if [ ! -f "$file" ]; then
    echo "  SKIP  $(basename "$file") (not in this worktree)"
    return 0
  fi
  before=$(cat "$file")
  sed -i '' "$expr" "$file"
  after=$(cat "$file")
  if [ "$before" = "$after" ]; then
    echo "  STALE $(basename "$file") — no line matched; it has drifted off the bump"
    return 1
  fi
  return 0
}

bumpfiles() {  # $1 = from, $2 = to;  non-zero if ANY present file was untouched
  local rc=0
  sed_verify "$CML" \
    "s/^project(EchoJay VERSION $1)\$/project(EchoJay VERSION $2)/" || rc=1
  sed_verify "$REPO/build-installer.sh" "s/^VERSION=\"$1\"\$/VERSION=\"$2\"/" || rc=1
  sed_verify "$REPO/package-dmg.sh"     "s/^VERSION=\"$1\"\$/VERSION=\"$2\"/" || rc=1
  return $rc
}

if ! bumpfiles "$OLD" "$NEW"; then
  # Undo whatever DID change, so a partial bump is not left behind. This
  # revert is best-effort by definition — it is unwinding a bump that was
  # already incomplete — so its own output is suppressed and its status
  # ignored; the abort below is the message that matters.
  bumpfiles "$NEW" "$OLD" >/dev/null 2>&1 || true
  echo "ABORT: version bump did not reach every file — nothing was built or installed."
  echo "  A file listed STALE above no longer contains VERSION=\"$OLD\"."
  echo "  Set it to match CMakeLists.txt by hand and the bump resumes on the next run."
  exit 1
fi
echo "Version bumped: $OLD -> $NEW  (display $DISPLAY)"

# 2b. Configure if the build tree was never set up (e.g. after `rm -rf build`).
#     Guarded on the cache so a normal incremental reinstall skips it. Uses this
#     tree's flags (Unix Makefiles, Release, universal arm64;x86_64, deploy 11.0,
#     compile-commands on). On failure, revert the bump so no number is burned.
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  echo "No CMake cache — configuring $BUILD ..."
  if ! cmake -S "$REPO" -B "$BUILD" -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0; then
    bumpfiles "$NEW" "$OLD"
    echo "ABORT: configure failed — version reverted to $OLD, nothing installed."
    exit 1
  fi
fi

# 3. Rebuild so the installed binary actually carries the new version.
#    On failure: revert the bump (no number burned) and leave the previously
#    installed component untouched — nothing half-installs.
#
#    PARALLELISM, and why it is stated rather than left to the default.
#    `cmake --build` passes NO -j of its own, so make runs SERIALLY unless
#    something supplies one. That is not a small tax on this project: a
#    universal (arm64;x86_64) Release build of both plugins measured about
#    FORTY MINUTES on one core, and it was paid silently on every reinstall
#    in every worktree, which is exactly the shape of cost nobody attributes
#    to its cause.
#
#    Defaults to the machine's core count. Two overrides, both deliberate:
#      EJ_JOBS=n     dial it down when a DAW is open — a full-core build
#                    will fight Logic for cores and can cost you dropouts.
#      MAKEFLAGS     an explicit -j from the caller still reaches make
#                    through the environment; the command-line -j below
#                    wins, so set EJ_JOBS if you want a specific number.
JOBS="${EJ_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
echo "Building with -j$JOBS ..."
if ! cmake --build "$BUILD" --config Release -j "$JOBS"; then
  bumpfiles "$NEW" "$OLD"
  echo "ABORT: build failed — version reverted to $OLD, nothing installed."
  exit 1
fi

# 3b. SELF-TESTS, between a green build and the install.
#
#     This is the moment that matters: a geometry or parity regression caught
#     here never reaches a DAW, and catching it here costs seconds. It is
#     deliberately NOT in the pre-commit hook, where doubling a 15 second gate
#     would be a tax on the wrong moment.
#
#     Each test is skipped, not failed, when the worktree does not have it, so
#     older branches still build. A failure aborts BEFORE the install and
#     reverts the version bump, exactly as a build failure does, so a red test
#     leaves the previously installed component in place rather than a
#     half-trusted new one.
for T in tabstrip_test art_parity_test dashboard_test linkmixer_test paramapply_test ringseek_test; do
  RUNNER="$REPO/tools/$T/build_and_run.sh"
  if [ ! -x "$RUNNER" ]; then
    echo "  SKIP  $T (not in this worktree)"
    continue
  fi
  echo "  running $T ..."
  if ! OUT=$("$RUNNER" 2>&1); then
    echo "$OUT" | tail -20
    bumpfiles "$NEW" "$OLD"
    echo "ABORT: $T FAILED. Version reverted to $OLD, nothing installed."
    exit 1
  fi
  echo "  ok    $T: $(echo "$OUT" | tail -1)"
done

# 4. Install (existing behaviour, unchanged).
for NAME in "EchoJay V2" "EchoJay Link"; do
  SRC=$(find "$BUILD" -type d -path "*/AU/$NAME.component" 2>/dev/null | head -1)
  [ -z "$SRC" ] && { echo "SKIP: $NAME.component not built yet"; continue; }
  echo "Installing $NAME  (version $(defaults read "$SRC/Contents/Info" CFBundleShortVersionString 2>/dev/null))"
  rm -rf "$DEST/$NAME.component"
  cp -R "$SRC" "$DEST/"
done
echo "Done ($DISPLAY). Reopen Logic."
