#!/bin/bash
# PACKAGE EJ MAP THE WAY A STRANGER GETS IT (ruled 2 Oct 2026, docs/STRANGER_MAC_TEST.md A1):
# one ejmap.app with EchoJayProbe inside Contents/MacOS, both signed with ONE Developer ID
# (hardened runtime, timestamp, the probe's entitlements), so `ejmap --cert-sweep-all` finds the
# probe beside its own executable with no --probe flag. Run ONCE per Mac that builds; after that
# the mapper uses only the app.
#
#   package_app.sh <built ejmap.app> <built EchoJayProbe> <out dir> "<Developer ID Application: Name (TEAM)>"
#
# Prints the two signatures and verifies the bundle strictly; exits non-zero on any failure.
set -euo pipefail
APP="${1:?built ejmap.app}"; PROBE="${2:?built EchoJayProbe}"; OUT="${3:?out dir}"; ID="${4:?Developer ID Application identity}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ENT="$HERE/../../au_instantiate_probe/EchoJayProbe.entitlements"
[ -d "$APP" ] || { echo "no app bundle at $APP"; exit 2; }
[ -f "$PROBE" ] || { echo "no probe at $PROBE"; exit 2; }
[ -f "$ENT" ] || { echo "no entitlements at $ENT"; exit 2; }
mkdir -p "$OUT"
DST="$OUT/ejmap.app"
rm -rf "$DST"
ditto "$APP" "$DST"
cp "$PROBE" "$DST/Contents/MacOS/EchoJayProbe"
chmod 755 "$DST/Contents/MacOS/EchoJayProbe"
# the probe first (a nested executable signs before the bundle that holds it), then the app
codesign -f -s "$ID" --timestamp --options runtime --entitlements "$ENT" "$DST/Contents/MacOS/EchoJayProbe"
codesign -f -s "$ID" --timestamp --options runtime --entitlements "$ENT" "$DST"
codesign --verify --strict --deep "$DST"
codesign --verify --strict "$DST/Contents/MacOS/EchoJayProbe"
echo "PACKAGED $DST"
for f in "$DST" "$DST/Contents/MacOS/EchoJayProbe"; do
  echo "--- $(basename "$f")"
  codesign -dvv "$f" 2>&1 | grep -E "^(Identifier|TeamIdentifier|Authority=Developer ID Application|CDHash|Timestamp)" | sed 's/^/    /'
done
echo "run:  \"$DST/Contents/MacOS/ejmap\" --cert-preflight"
