#!/bin/bash
# Place a ship folder from the build tree, and REFUSE to place a stale binary.
#
# 5 Oct 2026. Round 05a was placed from artefacts that were fourteen hours old: `cmake --build build-guards
# --target EchoJay EchoJayLink` builds the SHARED CODE static libraries, not the bundles. It printed
# "[100%] Built target EchoJay" and every assertion about it was true, while the four bundles on disk were
# still 04e's. Sean caught it from the UUID table - the AU arm64 UUIDs were identical to the build he already
# had installed. The four targets that actually produce bundles are EchoJay_AU, EchoJay_VST3, EchoJayLink_AU
# and EchoJayLink_VST3.
#
# Two refusals, because the UUID check alone would pass on a stale binary that was never installed:
#   (1) STALE AGAINST THE SOURCE: a placed binary older than the newest tracked source file cannot contain it.
#   (2) IDENTICAL TO WHAT IS INSTALLED, *AND* stale against its own sources - see below.
# Read-only against ~/Library: the installed bundles are read for their UUIDs and never written.
#
# 7 Oct 2026 - THE BLIND SPOT IN (2), and why the rule it was written with is false. (2) used to read "a rebuilt
# binary cannot keep its UUID, so a match means nothing was built". That is not true of a REPRODUCIBLE build whose
# inputs did not change: round 06c changed PluginEditor.cpp, PluginProcessor.cpp and EchoJayAPI.cpp, the EchoJay Link
# archive compiles NONE of the three, and the Link bundle therefore relinked bit-for-bit identical - correctly. (2)
# refused the whole folder for it, and the round was placed by hand, which is the one thing this script exists to
# stop. The failure was not the UUID comparison; it was comparing every bundle against the newest source file in the
# WHOLE tree, when what matters is the newest source THAT BUNDLE COMPILES. So both checks are now scoped per bundle,
# from the object list of the target that built it (CMakeFiles/<EchoJay|EchoJayLink>.dir), and an identical UUID is a
# REFUSAL only when that bundle is also stale against its own sources. Otherwise it is a NOTE that says so, because
# "this bundle did not change because nothing it compiles changed" is information, not a fault.
set -u
DEST="${1:?usage: place_ship.sh ship_YYYY-MM-DDx [build-dir]}"
BUILD="${2:-build-guards}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

A="$BUILD/EchoJay_artefacts/Release"
B="$BUILD/EchoJayLink_artefacts/Release"
declare -a SRC=( "$A/AU/EchoJay V2.component"   "$A/VST3/EchoJay V2.vst3"
                 "$B/AU/EchoJay Link.component" "$B/VST3/EchoJay Link.vst3" )
INSTALLED_AU="$HOME/Library/Audio/Plug-Ins/Components"

binIn() {   # the Mach-O inside a bundle
  local b="$1" n; n="$(basename "$b")"; n="${n%.*}"; printf '%s/Contents/MacOS/%s' "$b" "$n"
}
arm64Uuid() { dwarfdump --uuid "$1" 2>/dev/null | awk '/\(arm64\)/{print $2}'; }

fail=0
# Which shared-code target a bundle belongs to, and the newest SOURCE FILE THAT TARGET COMPILES.
# The object list is the authority on what a bundle contains: a .cpp with no .o in that target's
# object dir is not in that binary, so a change to it cannot make that binary stale. Falls back to
# the whole-tree rule when the object dir is missing, because a check that silently weakens itself
# is worse than one that is occasionally strict.
targetOf() { case "$1" in *Link*) printf 'EchoJayLink';; *) printf 'EchoJay';; esac; }
newestWholeTree=$(git ls-files -z 'Source/*' | xargs -0 stat -f '%m %N' | sort -rn | head -1)
echo "newest tracked source (whole tree): ${newestWholeTree#* }"

# newestFor <bundle> -> "<mtime> <path>  (scope)"
newestFor() {
  local tgt odir line
  tgt="$(targetOf "$1")"; odir="$BUILD/CMakeFiles/$tgt.dir/Source"
  if [ -d "$odir" ]; then
    # every .o in this target's object dir maps back to Source/<name>.cpp
    line=$(for o in "$odir"/*.o; do
             [ -e "$o" ] || continue
             b="$(basename "$o")"; src="Source/${b%.o}"
             [ -f "$src" ] && stat -f '%m %N' "$src"
           done | sort -rn | head -1)
    if [ -n "$line" ]; then printf '%s  (%s'"'"'s own %s objects)' "$line" "$tgt" "$(ls "$odir"/*.o 2>/dev/null | wc -l | tr -d ' ')"; return; fi
  fi
  printf '%s  (whole tree - no object dir for %s)' "$newestWholeTree" "$tgt"
}

declare -a UNCHANGED_OK=()
for s in "${SRC[@]}"; do
  if [ ! -e "$s" ]; then echo "REFUSED: missing artefact $s"; fail=1; continue; fi
  bin="$(binIn "$s")"
  binMs=$(stat -f '%m' "$bin")
  nf="$(newestFor "$s")"; nfMs=${nf%% *}; nfRest=${nf#* }
  if [ "$binMs" -lt "$nfMs" ]; then
    echo "REFUSED (stale): $(basename "$s") built $(date -r "$binMs" '+%d %b %H:%M'), older than $nfRest"
    echo "                 -> build the FOUR format targets: EchoJay_AU EchoJay_VST3 EchoJayLink_AU EchoJayLink_VST3"
    fail=1
  else
    UNCHANGED_OK+=("$(basename "$s")")
  fi
done

# (2) identical to the installed build - a REFUSAL only for a bundle that is also stale above.
for s in "${SRC[@]}"; do
  case "$s" in *.component) ;; *) continue ;; esac
  name="$(basename "$s")"
  inst="$INSTALLED_AU/$name"
  [ -e "$inst" ] || { echo "note: $name is not installed; no UUID comparison to make"; continue; }
  new="$(arm64Uuid "$(binIn "$s")")"; old="$(arm64Uuid "$(binIn "$inst")")"
  [ -n "$new" ] && [ "$new" = "$old" ] || continue
  fresh=0; for ok in ${UNCHANGED_OK+"${UNCHANGED_OK[@]}"}; do [ "$ok" = "$name" ] && fresh=1; done
  if [ "$fresh" -eq 1 ]; then
    echo "note: $name arm64 $new is the UUID ALREADY INSTALLED, and that is CORRECT here - the bundle"
    echo "      postdates every source it compiles, so nothing it contains changed and the build is"
    echo "      reproducible. Say so in the round's report; do not present it as a new binary."
  else
    echo "REFUSED (unchanged): $name arm64 $new is the UUID ALREADY INSTALLED and the bundle is stale"
    echo "                     against its own sources - nothing was rebuilt"
    fail=1
  fi
done

[ "$fail" -eq 0 ] || { echo; echo "NOT PLACED."; exit 1; }

rm -rf "$DEST"; mkdir -p "$DEST"
for s in "${SRC[@]}"; do cp -R "$s" "$DEST/" || exit 2; done
echo "placed in $DEST:"
for s in "${SRC[@]}"; do
  n="$(basename "$s")"
  echo "  $n  arm64 $(arm64Uuid "$(binIn "$DEST/$n")")  x86_64 $(dwarfdump --uuid "$(binIn "$DEST/$n")" | awk '/\(x86_64\)/{print $2}')"
done
