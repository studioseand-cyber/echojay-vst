#!/usr/bin/env bash
#
# which_build_is_installed.sh  —  answer "is the DAW running MY code?"
#
# The companion to install_local.sh, and the question that should be asked
# BEFORE re-debugging a fix that appears not to work.
#
# This repo is developed across seven git worktrees at once. Every one of them
# builds a bundle with the same identifier to the same install path, so the
# plugin a host loads is whichever worktree ran an install last — not whichever
# one you are editing. A fix can be correct, committed, compiled and tested, and
# still be nowhere near the process that is crashing.
#
# So this prints the installed bundle's Mach-O UUID, then searches every
# worktree's build tree for the artefact that UUID actually came from, and names
# it. That is the same comparison a crash report allows — a crash report lists
# the loaded image's UUID — which is what makes it worth having: run this, run
# nothing else, and either the installed build is yours or you have the path of
# the one that is.
#
# Usage:
#     tools/which_build_is_installed.sh
#
set -uo pipefail

ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"

uuid_of() {
    dwarfdump --uuid "$1" 2>/dev/null | awk '/\(arm64\)/ { print $2; exit }'
}

# The worktrees to search. `git worktree list` is the authority rather than a
# hardcoded list, so a worktree added later is covered without editing this.
worktrees() {
    git -C "$ROOT" worktree list --porcelain 2>/dev/null \
        | awk '/^worktree /{ print substr($0, 10) }'
}

report() {
    local label="$1" installed="$2" binname="$3"

    printf '\n== %s ==\n' "$label"
    if [[ ! -f "$installed" ]]; then
        printf '  not installed (%s)\n' "$installed"
        return
    fi

    local uuid
    uuid="$(uuid_of "$installed")"
    printf '  path:  %s\n' "$installed"
    printf '  built: %s\n' "$(stat -f '%Sm' -t '%F %T' "$installed")"
    printf '  uuid:  %s\n' "${uuid:-<unreadable>}"
    [[ -n "$uuid" ]] || return

    # Find the build tree that produced this exact binary.
    local found=""
    local wt bin d
    while IFS= read -r wt; do
        [[ -n "$wt" ]] || continue

        # EVERY build tree in the worktree, discovered — not a hardcoded pair.
        # This repo builds into build/, build-release/ AND build-dev/ (the
        # -DECHOJAY_DEV_TRANSPORT=ON build, which is the one actually being
        # tested). Naming only two of the three made an install from the third
        # report "NOT FOUND in any worktree build tree ... treat it as unknown
        # code" — the tool's own blind spot, printed in the voice of a verdict
        # about the binary. That is the exact failure this script exists to
        # prevent, so it must not be able to reintroduce it: discover the trees.
        local trees=()
        for d in "$wt"/build*; do
            [[ -d "$d" ]] && trees+=("$d")
        done
        [[ ${#trees[@]} -gt 0 ]] || continue

        while IFS= read -r bin; do
            [[ -n "$bin" ]] || continue
            if [[ "$(uuid_of "$bin")" == "$uuid" ]]; then
                found="$wt"
                printf '  origin: %s\n' "$bin"
                break 2
            fi
        # Depth 8 clears the real artefact layout with room to spare:
        #   build/EchoJay_artefacts/<config>/AU/<bundle>/Contents/MacOS/<bin>
        # is already 7, and a shallower cap silently finds nothing — which reads
        # exactly like "installed from a build that no longer exists".
        done < <(find "${trees[@]}" -maxdepth 8 \
                      -path "*MacOS/$binname" -type f 2>/dev/null)
    done < <(worktrees)

    if [[ -z "$found" ]]; then
        printf '  origin: NOT FOUND in any worktree build tree\n'
        printf '          (installed from a build that has since been cleaned or\n'
        printf '           rebuilt — treat it as unknown code)\n'
        return
    fi

    printf '  branch: %s @ %s\n' \
        "$(git -C "$found" rev-parse --abbrev-ref HEAD 2>/dev/null)" \
        "$(git -C "$found" rev-parse --short HEAD 2>/dev/null)"

    if [[ "$found" == "$ROOT" ]]; then
        printf '  VERDICT: this worktree. A host is running YOUR code.\n'
    else
        printf '  VERDICT: *** %s — NOT this worktree. ***\n' "$(basename "$found")"
        printf '           A host is running that build, not the one you are editing.\n'
        printf '           Rebuild here, then: tools/install_local.sh\n'
    fi
}

printf 'this worktree: %s (%s @ %s)\n' \
    "$ROOT" \
    "$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)" \
    "$(git -C "$ROOT" rev-parse --short HEAD)"

report "AudioUnit" \
    "$HOME/Library/Audio/Plug-Ins/Components/EchoJay V2.component/Contents/MacOS/EchoJay V2" \
    "EchoJay V2"

report "VST3" \
    "$HOME/Library/Audio/Plug-Ins/VST3/EchoJay V2.vst3/Contents/MacOS/EchoJay V2" \
    "EchoJay V2"

# A system-wide copy competes for the same identifier and is resolved by host
# cache rather than by rule, so it is surfaced here too.
for shadow in "/Library/Audio/Plug-Ins/Components/EchoJay V2.component" \
              "/Library/Audio/Plug-Ins/VST3/EchoJay V2.vst3"; do
    [[ -e "$shadow" ]] && printf '\nWARNING: system-wide copy also present: %s\n' "$shadow"
done

exit 0
