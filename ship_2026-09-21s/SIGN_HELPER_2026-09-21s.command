#!/bin/bash
# ship_2026-09-21s - the signing steps IN ORDER, both bundles:
#   1. codesign the nested EchoJayProbe helper FIRST (hardened runtime + the disable-library-validation + allow-unsigned-executable-memory entitlements it
#      needs to dlopen third-party AUs) - this script.
#   2. wraptool sign ... on each bundle as before (PACE), which signs the bundle over the already-signed helper.
#   3. codesign --verify --deep --strict on each bundle: must print "valid on disk" and "satisfies its Designated Requirement".
set -e; HERE="$(cd "$(dirname "$0")" && pwd)"; DIR="${1:-$HERE}"
ID="Developer ID Application: Sean Donoghue (8BT5F9B887)"
for b in "EchoJay V2.aaxplugin" "EchoJay Link.aaxplugin"; do
  H="$DIR/$b/Contents/MacOS/EchoJayProbe"
  codesign -f -s "$ID" --timestamp --options runtime --entitlements "$HERE/EchoJayProbe.entitlements" "$H"
  codesign -dv --entitlements - "$H" 2>&1 | grep -E "Authority=Developer|disable-library-validation|allow-unsigned-executable-memory" | head -2
done
echo "step 1 done: helpers signed. Step 2: wraptool sign ... on each bundle. Step 3:"
echo "  codesign --verify --deep --strict \"$DIR/EchoJay V2.aaxplugin\" && codesign --verify --deep --strict \"$DIR/EchoJay Link.aaxplugin\" && echo VERIFY OK"
