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
    # ---- AND A SOURCE THE BUNDLE DOES NOT COMPILE IS NOT STALENESS (26 Oct 2026) -------------
    # This counted every file under Source/, so a V2-only .cpp refused a Link bundle that was
    # perfectly current - twice today, and the second time it installed the V2 half of a two-sided
    # fix and left the Link half behind, which is worse than refusing both. A header still counts
    # for everything (it can change any object's layout); a .cpp or .mm counts only for the target
    # that actually compiled it, read off that target's own object list in this build tree. Same
    # rule harness_build.py has had since 4 Oct, and for the same reason.
    local objdir="" stale=""
    case "$label" in
        Link*) objdir="$BUILD_DIR/CMakeFiles/EchoJayLink.dir/Source" ;;   # BUILD_DIR is absolute by here (line 58)
        *)     objdir="$BUILD_DIR/CMakeFiles/EchoJay.dir/Source" ;;
    esac
    while IFS= read -r f; do
        [[ -z "$f" ]] && continue
        case "$f" in
            *.cpp|*.mm|*.c)
                # compiled into THIS target? if there is no object for it, it cannot have changed it
                if [[ -d "$objdir" ]]; then
                    [[ -f "$objdir/$(basename "$f").o" ]] || continue
                else
                    note "         (no object list at $objdir - every source counts for this bundle)"
                fi ;;
        esac
        stale="$f"; break
    done < <(find "$ROOT/Source" -type f -newer "$src_bin" 2>/dev/null)
    if [[ -n "$stale" ]]; then
        note "REFUSED: this bundle is older than the source tree"
        note "         bundle: $(date -r "$src_bin" '+%d %b %H:%M')  (from $BUILD_DIR)"
        note "         newer:  $(basename "$stale")  $(date -r "$stale" '+%d %b %H:%M')"
        note "         build this tree first, or name the build dir that holds"
        note "         the bundles: tools/install_local.sh build-guards"
        fail=1
        return 0
    fi

    # ---- AND NO INSTALL CLAIMS GREEN ON ITS OWN (C's point 80, 29 Oct 2026) -----------------
    #
    # mapfps_test was RED for thirteen days unseen, because every round since 16 Oct ran "touched
    # guards only" and that suite was never touched - so "touched guards: GREEN" stood in for "the
    # build is green" at every install in between. C's shape for it is exact: a green leg against
    # a stub.
    #
    # So the bundles must be accompanied by a verdict written by the round that gated them
    # (tools/gate_verdict.sh): GREEN, naming its suites, and NEWER than the binary it is about. A
    # verdict about an earlier build is about an earlier build.
    #
    # EJ_INSTALL_NO_VERDICT=1 is the deliberate override and it SAYS SO in the output, so an
    # install without a gate behind it is visible in the record rather than indistinguishable from
    # one with.
    local vfile="$BUILD_DIR/GATE_VERDICT.txt"
    if [[ "${EJ_INSTALL_NO_VERDICT:-0}" == "1" ]]; then
        note "         (NO VERDICT CHECKED: EJ_INSTALL_NO_VERDICT=1 - this install has no gate behind it)"
    elif [[ ! -f "$vfile" ]]; then
        note "REFUSED: no GATE_VERDICT.txt beside these bundles"
        note "         the round that built them must write one: tools/gate_verdict.sh $BUILD_DIR GREEN \"<suite>: <result>\"..."
        note "         (C's point 80: \"touched guards green\" stood in for \"the build is green\" for 13 days)"
        fail=1
        return 0
    elif grep -q "^verdict: AHEAD" "$vfile" && [[ -n "${EJ_INSTALL_AHEAD:-}" ]]; then
        # ---- AN INSTALL AHEAD OF ITS SUITES, SAID OUT LOUD (C's point 89, 30 Oct 2026) -------
        # AHEAD means nothing that ran is red and some suites have NOT run on this fingerprint.
        # It exists because a HANG had to reach him faster than a 35-minute suite could, and the
        # alternative was writing GREEN over a page of NOT RUN - which is the fault this file was
        # written to stop. The flag is the deliberate act; these lines are the record.
        note "INSTALLING AHEAD OF THE SUITES, because EJ_INSTALL_AHEAD is set."
        note "         the verdict names the suites that have NOT run on this build. Read them:"
        sed 's/^/         /' "$vfile" | head -14 | while IFS= read -r l; do note "$l"; done
    elif ! grep -q "^verdict: GREEN" "$vfile"; then
        note "REFUSED: the gate verdict beside these bundles is not GREEN"
        grep -q "^verdict: AHEAD" "$vfile" \
            && note "         it is AHEAD of its suites - set EJ_INSTALL_AHEAD=1 if that is what you mean"
        sed 's/^/         /' "$vfile" | head -12 | while IFS= read -r l; do note "$l"; done
        fail=1
        return 0
    elif [[ "$vfile" -ot "$src_bin" ]]; then
        note "REFUSED: the gate verdict is OLDER than the bundle it is supposed to be about"
        note "         verdict: $(date -r "$vfile" '+%d %b %H:%M')   bundle: $(date -r "$src_bin" '+%d %b %H:%M')"
        note "         re-run the gate for this build, or write its verdict after it"
        fail=1
        return 0
    else
        # ---- AND IT MUST BE ABOUT THESE SOURCES (C's point 82, 29 Oct 2026) -----------------
        #
        # GREEN and newer are not enough: a verdict written from a gate run on a DIFFERENT tree
        # passes both and says nothing true about the bytes in this bundle. The 15:42 verdict
        # recorded "tree: 31 file(s) uncommitted", which is exactly the shape of that risk.
        #
        # So the verdict carries a fingerprint of every compiled source by CONTENT, and this
        # compares it with the tree as it stands. A mismatch is not necessarily a stale verdict -
        # it can equally be a source edited after the gate - and either way the honest answer is
        # the same: the gate has not judged what is about to be installed.
        local vfp tfp
        vfp="$(grep '^sources:' "$vfile" | awk '{print $2}')"
        tfp="$(cd "$ROOT" && find Source -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' \) \
                 -print0 2>/dev/null | sort -z | xargs -0 shasum -a 256 2>/dev/null \
                 | shasum -a 256 | cut -c1-16)"
        if [[ -z "$vfp" ]]; then
            note "         (this verdict predates the source fingerprint - it is accepted, and the"
            note "          next gate run will carry one; C's point 82)"
        elif [[ "$vfp" != "$tfp" ]]; then
            note "REFUSED: the gate verdict is about DIFFERENT sources than the tree holds now"
            note "         verdict's sources: $vfp"
            note "         this tree's:       $tfp"
            note "         either a source changed after the gate, or that verdict came from another"
            note "         tree. Re-gate this tree; GREEN has to mean green for these bytes."
            fail=1
            return 0
        fi
        note "         gate: $(grep '^verdict:' "$vfile" | head -1), written $(date -r "$vfile" '+%d %b %H:%M'), $(grep -c '^  ' "$vfile") suite line(s)$([[ -n "$vfp" ]] && echo ", sources $vfp")"
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
