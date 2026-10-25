#!/usr/bin/env bash
#
# install_local.sh  —  install THIS worktree's plugin, then PROVE it is the one
# a host will load.
#
# WHY THIS EXISTS
#
# The built-in device editors aborted Logic on open. The bug was found and fixed
# in Source/DeviceEditorBase.cpp, the tests went green, the plugin was rebuilt
# and reinstalled — and Logic kept crashing, in exactly the same place, for hours.
#
# The fix was never wrong. It was never LOADED. This repo is developed in seven
# git worktrees in parallel (see `git worktree list`), every one of them builds a
# bundle with the same CFBundleIdentifier — com.echojay.plugin.v2 — to the same
# two install paths. There is no per-worktree suffix and no versioning between
# them, so "install" means "overwrite whatever the last worktree put there", and
# "reinstall" from the wrong directory is indistinguishable from a fix that did
# not work. What Logic actually had loaded was a Release build from ../ej-time,
# made six minutes BEFORE the fix was committed, on a branch that does not
# contain it. Every symptom followed from that: the headless harness passed
# because it was built here, from fixed source, and the DAW crashed because it
# was running someone else's binary.
#
# A build and an install are cheap. Knowing WHICH build is installed is the part
# that was missing, so that is what this script exists to establish. It installs
# from a named worktree and then compares the Mach-O UUID of what it just wrote
# against what is now on disk — the same identifier a crash report prints, so a
# future crash can be tied to a build in one step instead of an afternoon.
#
# Usage:
#     tools/install_local.sh [BUILD_DIR]      # default: build-release
#
set -uo pipefail

ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
BUILD_DIR="${1:-build-release}"

# ---- IT REFUSES UNDER A LIVE HOST, AND IT NEVER HAS (25 Oct 2026) ---------------------------
# quick.sh checked `pgrep -x "Logic Pro"` before calling this; THIS script, which is what every
# other road and every hand-typed install uses, checked nothing at all. On the evening of 25 Oct
# Logic was closed and PRO TOOLS was open with his session and EchoJay in it, so an install would
# have copied new bundles straight over binaries a live host held open - the exact state behind the
# "fix that did not work" hunt this file's own header describes. One test, in the one place every
# road passes through. EJ_INSTALL_ANYWAY=1 is for a deliberate override and says so in the log.
EJ_LIVE_HOSTS="Pro Tools|Logic Pro|Ableton Live|Cubase|REAPER|AUHostingService|AAXHostService|AAEHostService"
LIVE_PIDS="$(pgrep -f "$EJ_LIVE_HOSTS" 2>/dev/null | tr '\n' ' ')"
if [[ -n "$LIVE_PIDS" && "${EJ_INSTALL_ANYWAY:-0}" != "1" ]]; then
    echo "REFUSED: a live plugin host is running (pid(s) $LIVE_PIDS)."
    echo "         Installing now would overwrite bundles it holds open, and the host would keep"
    echo "         the old binary - which is the stale-binary hunt this script's header is about."
    echo "         Close the host and run it again, or EJ_INSTALL_ANYWAY=1 to override deliberately."
    exit 2
fi
if [[ -n "$LIVE_PIDS" ]]; then
    echo "NOTE: a live plugin host is running (pid(s) $LIVE_PIDS) and EJ_INSTALL_ANYWAY=1 was set -"
    echo "      installing anyway, deliberately. Verify the running binary by its UUID afterwards."
fi
[[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$ROOT/$BUILD_DIR"

AU_DEST="$HOME/Library/Audio/Plug-Ins/Components"
VST3_DEST="$HOME/Library/Audio/Plug-Ins/VST3"

fail=0
note() { printf '  %s\n' "$1"; }

# uuid_of <binary> — the arm64 slice's UUID. Universal binaries carry one per
# slice; arm64 is what a crash report on Apple silicon names, so that is the one
# worth comparing.
uuid_of() {
    dwarfdump --uuid "$1" 2>/dev/null \
        | awk '/\(arm64\)/ { print $2; exit }'
}

# Find the artefact regardless of which config the build dir was set up as, so
# this works against a Debug tree too rather than silently finding nothing.
find_artefact() {
    local pattern="$1"
    find "$BUILD_DIR" -maxdepth 4 -name "$pattern" -print 2>/dev/null | head -1
}

printf '\n== installing from %s ==\n' "$ROOT"
note "branch:    $(git -C "$ROOT" rev-parse --abbrev-ref HEAD)"
note "commit:    $(git -C "$ROOT" rev-parse --short HEAD)"
note "build dir: $BUILD_DIR"

if [[ ! -d "$BUILD_DIR" ]]; then
    printf '\nFAIL: no build dir at %s\n' "$BUILD_DIR"
    printf '      cmake -S . -B %s -DCMAKE_BUILD_TYPE=Release && cmake --build %s --target EchoJay_AU EchoJay_VST3 EchoJayLink_AU EchoJayLink_VST3 -j 4\n' \
        "$(basename "$BUILD_DIR")" "$(basename "$BUILD_DIR")"
    exit 1
fi

# ---------------------------------------------------------------------------
# A copy in /Library shadows nothing and conflicts with everything: both trees
# are scanned, so two bundles claiming one identifier is an ambiguity resolved
# by whatever the host cached. It has bitten this project before, so it is
# reported rather than assumed absent.
for shadow in "/Library/Audio/Plug-Ins/Components/EchoJay V2.component" \
              "/Library/Audio/Plug-Ins/VST3/EchoJay V2.vst3"; do
    if [[ -e "$shadow" ]]; then
        printf '\nWARNING: a system-wide copy also exists and will compete for the same id:\n'
        note "$shadow"
        note "remove it with: sudo rm -rf \"$shadow\""
        fail=1
    fi
done

# ---------------------------------------------------------------------------
install_one() {
    local label="$1" src="$2" dest_dir="$3"

    printf '\n-- %s --\n' "$label"
    if [[ -z "$src" || ! -d "$src" ]]; then
        note "SKIP: not built in $BUILD_DIR"
        return 0
    fi

    local bundle binname
    bundle="$(basename "$src")"
    binname="$(basename "${bundle%.*}")"

    local src_bin="$src/Contents/MacOS/$binname"
    if [[ ! -f "$src_bin" ]]; then
        note "FAIL: no binary inside $src"
        fail=1
        return 0
    fi

    # 24 Oct: a chain script called this with no BUILD_DIR, so the default
    # (build-release) installed three-week-old bundles over the build under
    # test. The bundles are produced in build-guards; build-release carries
    # only the archives the guards link. A binary older than the source it is
    # supposed to contain is never what the caller meant, so it is refused
    # here rather than written to the host's search path.
    local stale
    stale="$(find "$ROOT/Source" -type f -newer "$src_bin" -print -quit 2>/dev/null)"
    if [[ -n "$stale" ]]; then
        note "REFUSED: this bundle is older than the source tree"
        note "         bundle: $(date -r "$src_bin" '+%d %b %H:%M')  (from $BUILD_DIR)"
        note "         newer:  $(basename "$stale")  $(date -r "$stale" '+%d %b %H:%M')"
        note "         build this tree first, or name the build dir that holds"
        note "         the bundles: tools/install_local.sh build-guards"
        fail=1
        return 0
    fi

    local want
    want="$(uuid_of "$src_bin")"

    mkdir -p "$dest_dir"
    # ditto, not cp -R: it replaces the bundle wholesale rather than merging into
    # a previous install, so a file that only the OLD build had cannot survive.
    rm -rf "$dest_dir/$bundle"
    if ! ditto "$src" "$dest_dir/$bundle"; then
        note "FAIL: could not install to $dest_dir/$bundle"
        fail=1
        return 0
    fi

    local got
    got="$(uuid_of "$dest_dir/$bundle/Contents/MacOS/$binname")"

    note "source:    $src"
    note "installed: $dest_dir/$bundle"
    note "uuid:      ${got:-<none>}"

    if [[ -z "$want" || "$want" != "$got" ]]; then
        note "FAIL: installed UUID does not match the build it came from"
        fail=1
    fi
}

AU_SRC="$(find_artefact 'EchoJay V2.component')"
VST3_SRC="$(find_artefact 'EchoJay V2.vst3')"

install_one "AudioUnit" "$AU_SRC"   "$AU_DEST"
install_one "VST3"      "$VST3_SRC" "$VST3_DEST"

# Round 53: the LINK is part of the same build (its sidecar carries the
# publisher's pid and host identity that the main's alignment budget now
# requires - an old Link never counts). It is installed with the main.
LINK_AU_SRC="$(find_artefact 'EchoJay Link.component')"
LINK_VST3_SRC="$(find_artefact 'EchoJay Link.vst3')"
install_one "Link AudioUnit" "$LINK_AU_SRC"   "$AU_DEST"
install_one "Link VST3"      "$LINK_VST3_SRC" "$VST3_DEST"

# ---------------------------------------------------------------------------
# The AU cache maps a component id to a scanned binary. Leaving it in place is
# the other way a host ends up on code that is no longer on disk.
rm -rf "$HOME/Library/Caches/AudioUnitCache" 2>/dev/null
printf '\n-- AudioUnitCache cleared --\n'
note "hosts will rescan on next launch"

# ---------------------------------------------------------------------------
# Name the UUID that is now live, so a crash report can be matched to a build
# without repeating the search that found this bug.
printf '\n== what a host will now load ==\n'
if [[ -f "$AU_DEST/EchoJay V2.component/Contents/MacOS/EchoJay V2" ]]; then
    note "AU   $(uuid_of "$AU_DEST/EchoJay V2.component/Contents/MacOS/EchoJay V2")"
fi
if [[ -f "$VST3_DEST/EchoJay V2.vst3/Contents/MacOS/EchoJay V2" ]]; then
    note "VST3 $(uuid_of "$VST3_DEST/EchoJay V2.vst3/Contents/MacOS/EchoJay V2")"
fi
if [[ -f "$AU_DEST/EchoJay Link.component/Contents/MacOS/EchoJay Link" ]]; then
    note "Link AU   $(uuid_of "$AU_DEST/EchoJay Link.component/Contents/MacOS/EchoJay Link")"
fi
if [[ -f "$VST3_DEST/EchoJay Link.vst3/Contents/MacOS/EchoJay Link" ]]; then
    note "Link VST3 $(uuid_of "$VST3_DEST/EchoJay Link.vst3/Contents/MacOS/EchoJay Link")"
fi
note "compare against a crash report's \"EchoJay V2\" image UUID before believing"
note "a fix did not work."

if (( fail )); then
    printf '\nINSTALL REPORTED PROBLEMS\n'
    exit 1
fi
printf '\nINSTALL OK\n'
