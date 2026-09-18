#!/bin/bash
# 18 Sep 2026 - sign the nested pre-flight helper BEFORE wraptool signs each bundle (the nested Mach-O must be
# signed for `codesign --verify --deep --strict` to pass, and it needs disable-library-validation to dlopen
# third-party AUs under the hardened runtime - measured on Acme Opticom XLA-3: without it "OS error (-1)").
#   bash SIGN_HELPER_2026-09-18.command [dir holding the two bundles]   (default: this folder)
set -e; HERE="$(cd "$(dirname "$0")" && pwd)"; DIR="${1:-$HERE}"
ID="Developer ID Application: Sean Donoghue (8BT5F9B887)"
for b in "EchoJay V2.aaxplugin" "EchoJay Link.aaxplugin"; do
  H="$DIR/$b/Contents/MacOS/EchoJayProbe"
  codesign -f -s "$ID" --timestamp --options runtime --entitlements "$HERE/EchoJayProbe.entitlements" "$H"
  codesign -dv --entitlements - "$H" 2>&1 | grep -E "Authority=Developer|disable-library-validation" | head -2
done
echo "helpers signed. Now: wraptool sign ... on each bundle as before, then:"
echo "  codesign --verify --deep --strict \"$DIR/EchoJay V2.aaxplugin\" && codesign --verify --deep --strict \"$DIR/EchoJay Link.aaxplugin\" && echo VERIFY OK"
