#!/bin/bash
# ship_2026-09-21t-a - the signing steps IN ORDER. V2 ONLY this round: 21t-a changed the V2 editor and the V2
# processor, no Link source, so the Link binary is 21s's and is neither re-signed nor re-installed.
#   1. codesign the nested EchoJayProbe helper FIRST (hardened runtime + the disable-library-validation and
#      allow-unsigned-executable-memory entitlements it needs to dlopen third-party AUs) - this script.
#   2. wraptool sign ... on the V2 bundle as before (PACE), which signs the bundle over the already-signed helper.
#   3. codesign --verify --deep --strict on the bundle: must print "valid on disk" and "satisfies its Designated
#      Requirement".
set -e; HERE="$(cd "$(dirname "$0")" && pwd)"; DIR="${1:-$HERE}"
ID="Developer ID Application: Sean Donoghue (8BT5F9B887)"
b="EchoJay V2.aaxplugin"
H="$DIR/$b/Contents/MacOS/EchoJayProbe"
codesign -f -s "$ID" --timestamp --options runtime --entitlements "$HERE/EchoJayProbe.entitlements" "$H"
codesign -dv --entitlements - "$H" 2>&1 | grep -E "Authority=Developer|disable-library-validation|allow-unsigned-executable-memory" | head -2
echo "step 1 done: the V2 helper is signed. Step 2: wraptool sign ... on \"$DIR/$b\". Step 3:"
echo "  codesign --verify --deep --strict \"$DIR/$b\" && echo VERIFY OK"
