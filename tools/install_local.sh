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
note "compare against a crash report's \"EchoJay V2\" image UUID before believing"
note "a fix did not work."

if (( fail )); then
    printf '\nINSTALL REPORTED PROBLEMS\n'
    exit 1
fi
printf '\nINSTALL OK\n'
