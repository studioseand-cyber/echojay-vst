#!/bin/bash
# ship_2026-09-20h: sign the nested EchoJayProbe helper FIRST (hardened runtime + the disable-library-validation
# entitlement it needs to dlopen third-party AUs), then wraptool the bundle, then deep-strict verify. V2 only.
set -e; HERE="$(cd "$(dirname "$0")" && pwd)"; DIR="${1:-$HERE}"
ID="Developer ID Application: Sean Donoghue (8BT5F9B887)"
H="$DIR/EchoJay V2.aaxplugin/Contents/MacOS/EchoJayProbe"
codesign -f -s "$ID" --timestamp --options runtime --entitlements "$HERE/EchoJayProbe.entitlements" "$H"
codesign -dv --entitlements - "$H" 2>&1 | grep -E "Authority=Developer|disable-library-validation" | head -2
echo "helper signed. Now: wraptool sign ... on the V2 bundle as before, then:"
echo "  codesign --verify --deep --strict \"$DIR/EchoJay V2.aaxplugin\" && echo VERIFY OK"
