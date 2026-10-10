#!/bin/bash
# ship_2026-09-21t-j - the signing steps IN ORDER, for BOTH bundles. ON TOP OF THE INSTALLED 21t-i: the AAX
# folder reads V2 453957B5 / 7A302436 and Link DAF8007A / 7DEB0AD8, which are 21t-i's four UUIDs exactly, so
# 21t-i IS what is running (my round notes said it was never signed; the disk says otherwise, and the disk
# wins). What 21t-j adds: the SETTLE (a
# build posts one line and finishes it in place once it has heard and landed, at most three steps in 15 s
# of heard audio); the -70 LUFS absolute gate on every loudness figure the UI draws; groups of 64 with
# nothing dropped silently; registration as a full snapshot, so a copied insert's role reaches V2; the
# GR-meter cross-check in the log; and key/reference attributed to the [KEY] block's own source.
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
