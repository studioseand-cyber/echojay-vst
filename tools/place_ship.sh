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
#   (2) IDENTICAL TO WHAT IS INSTALLED: a rebuilt binary cannot keep its UUID, so a match means nothing was built.
# Read-only against ~/Library: the installed bundles are read for their UUIDs and never written.
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
# The newest tracked source file: what any honest binary must postdate.
newest=$(git ls-files -z 'Source/*' | xargs -0 stat -f '%m %N' | sort -rn | head -1)
newestMs=${newest%% *}; newestName=${newest#* }
echo "newest tracked source: $newestName"

for s in "${SRC[@]}"; do
  if [ ! -e "$s" ]; then echo "REFUSED: missing artefact $s"; fail=1; continue; fi
  bin="$(binIn "$s")"
  binMs=$(stat -f '%m' "$bin")
  if [ "$binMs" -lt "$newestMs" ]; then
    echo "REFUSED (stale): $(basename "$s") built $(date -r "$binMs" '+%d %b %H:%M'), older than $newestName"
    echo "                 -> build the FOUR format targets: EchoJay_AU EchoJay_VST3 EchoJayLink_AU EchoJayLink_VST3"
    fail=1
  fi
done

# (2) identical to the installed build
for s in "${SRC[@]}"; do
  case "$s" in *.component) ;; *) continue ;; esac
  name="$(basename "$s")"
  inst="$INSTALLED_AU/$name"
  [ -e "$inst" ] || { echo "note: $name is not installed; no UUID comparison to make"; continue; }
  new="$(arm64Uuid "$(binIn "$s")")"; old="$(arm64Uuid "$(binIn "$inst")")"
  if [ -n "$new" ] && [ "$new" = "$old" ]; then
    echo "REFUSED (unchanged): $name arm64 $new is the UUID ALREADY INSTALLED - nothing was rebuilt"
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
