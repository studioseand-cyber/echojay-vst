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

# Is the DEV TRANSPORT compiled into this binary?
#
# THE MORE DANGEROUS FAILURE THIS FILE EXISTS FOR (4 Aug 2026). The UUID
# verdict below answers "is this MY build". It does NOT answer "which server
# is it talking to", and those are separate: a binary built from the right
# worktree, on the right branch, at the right commit, with ECHOJAY_DEV_TRANSPORT
# off reads GREEN on every line above while Logic quietly talks to PRODUCTION.
# ~/.echojay/dev.json is then silently ignored and a preview-only feature looks
# broken rather than absent — which is indistinguishable, from the DAW, from
# the feature not working.
#
# RAW BYTE SEARCH, NOT strings (standing rule, CHAIN_AI_BUILD_SPEC: learned
# twice on 1 Aug 2026). strings coalesces adjacent literals from the cstring
# pool onto one line, and LTO splits literals longer than 8 bytes across
# fragments — so a strings|grep can miss a marker that is genuinely present.
# "dev.json" is exactly 8 bytes, which is the longest a marker can be and
# still be safe from that split, and it is searched in the file itself.
#
# The marker is the dev.json path literal in EchoJayAPI.cpp's devTransport(),
# which is compiled out entirely when the flag is off — so its presence is the
# code being there, not merely a string that mentions it.
has_dev_transport() {
    LC_ALL=C grep -qa 'dev\.json' "$1"
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

    # BEFORE the origin search, deliberately: this answers a question the UUID
    # cannot, so it must print even when the origin is unresolvable or the
    # dwarfdump read fails. Both early returns below would otherwise take it
    # with them, and the case where you most need to know which server you are
    # talking to is the case where something else is already odd.
    if has_dev_transport "$installed"; then
        printf '  transport: DEV — reads ~/.echojay/dev.json, so a baseUrl there\n'
        printf '             redirects every call (login included).\n'
    else
        printf '  transport: *** PRODUCTION — dev transport NOT compiled in. ***\n'
        printf '             ~/.echojay/dev.json is IGNORED. A preview-only feature\n'
        printf '             will look BROKEN here rather than absent.\n'
        printf '             Rebuild with -DECHOJAY_DEV_TRANSPORT=ON (Debug gets it free).\n'
    fi

    [[ -n "$uuid" ]] || return

    # Find the build tree that produced this exact binary.
    local found=""
    local wt bin
    while IFS= read -r wt; do
        [[ -n "$wt" ]] || continue
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
        done < <(find "$wt/build" "$wt/build-release" -maxdepth 8 \
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
