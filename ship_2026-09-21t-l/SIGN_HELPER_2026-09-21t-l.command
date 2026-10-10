#!/bin/bash
# ship_2026-09-21t-l - the signing steps IN ORDER, for BOTH bundles. ON TOP OF THE INSTALLED 21t-k: the AAX
# folder reads V2 49F30609 / AA0C2737 and Link E53D3A89 / 13D36AE3, which are 21t-k's four UUIDs exactly.
# 21t-l is the nine things Sean found testing 21t-k: a group strip shows its COUNT with the names in a tooltip;
# a group fader's undo restores the group's own fader; key_source is true or reads "chat"; correction_mode never
# reads "custom" after a server build; a calibration block on an edit turn waits for the plugin; the [KEY] block
# sends what Meters shows; the group repair waits for a live Link; a level match writes a borrowed member's trim
# directly; and the headroom cap is the minimum true-peak room across every member - the headroom op applies now.
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
