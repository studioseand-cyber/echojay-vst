#!/bin/bash
# ship_2026-09-21t-i - the signing steps IN ORDER, for BOTH bundles. On top of the installed 21t-g
# (21t-h was packaged and never signed; it is included here). The compressor is set once at the build
# and then reports and asks; every chat block is composed from a stored per-channel level record that
# survives a transport stop and a session reopen; the level-match card carries Apply on every route.
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
