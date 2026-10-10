#!/bin/bash
# ship_2026-09-21t-k - the signing steps IN ORDER, for BOTH bundles. ON TOP OF THE INSTALLED 21t-j: the AAX
# folder reads V2 667B3D46 / 9C359AF2 and Link 2D14A17D / 85E4270D, which are 21t-j's four UUIDs exactly (it was
# 21t-i when this round started; 21t-j was signed and installed during it). 21t-k: the uid is stable across a reopen so groups survive it; the tuner's key and
# reference say where they came from; the compressor build refuses a contradictory block, settles over 3 steps
# and measures the plugin rather than the staging; and the Link Mixer gains range-select, select-by-role, a
# fader reset, a VCA group fader with one undo step, and labels that never overflow.
#   1. codesign the nested EchoJayProbe helper in EACH bundle FIRST (hardened runtime + the
#      disable-library-validation and allow-unsigned-executable-memory entitlements it needs to dlopen
#      third-party AUs) - this script.
#   2. wraptool sign ... on each bundle as before (PACE), which signs the bundle over the already-signed helper.
#   3. codesign --verify --deep --strict on each bundle: "valid on disk" and "satisfies its Designated Requirement".
set -e; HERE="$(cd "$(dirname "$0")" && pwd)"; DIR="${1:-$HERE}"
ID="Developer ID Application: Sean Donoghue (8BT5F9B887)"
for b in "EchoJay V2.aaxplugin" "EchoJay Link.aaxplugin"; do
  H="$DIR/$b/Contents/MacOS/EchoJayProbe"
  [ -f "$H" ] || { echo "STOP: no nested probe at $H"; exit 2; }
  codesign -f -s "$ID" --timestamp --options runtime --entitlements "$HERE/EchoJayProbe.entitlements" "$H"
  echo "signed helper in $b:"
  codesign -dv --entitlements - "$H" 2>&1 | grep -E "Authority=Developer|disable-library-validation|allow-unsigned-executable-memory" | head -3
done
echo "step 1 done: BOTH helpers signed. Step 2: wraptool sign ... on each bundle. Step 3:"
echo "  codesign --verify --deep --strict \"$DIR/EchoJay V2.aaxplugin\" && codesign --verify --deep --strict \"$DIR/EchoJay Link.aaxplugin\" && echo VERIFY OK"
