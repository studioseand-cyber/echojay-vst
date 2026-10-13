#!/bin/bash
# Assemble the EchoJay Wizard app with its engine: the app from the build, `ejmap` (the wizard engine) and the Developer ID signed
# EchoJayProbe beside it in Contents/MacOS, and the mock DB's references in Contents/Resources.
#   tools/ejmap/wizard_app/assemble.sh <build dir> <output folder> [--beta] [--sign "<Developer ID Application: Name (TEAM)>"] [--dmg]
#     --beta  the beta build (EchoJay Wizard Beta.app, target ejmap-wizard-beta: no sign-in, the results saved as one file to email)
#     --sign  (13 Oct, beta item 5) the probe, the engine and the app signed with the team's Developer ID, hardened runtime, timestamp -
#             the probe and the engine with the probe's entitlements (they host / launch plugins), nested executables before the bundle;
#             without --sign: the engine and the app ad hoc, as before (the probe must already be Developer ID signed)
#     --dmg   also a drag-to-Applications disk image beside the app (signed with --sign)
# Notarisation is NOT done: it waits on Apple's agreement (docs/NOTARISATION_SETUP.md). Testers clear the quarantine flag once
# (wizard_app/README_BETA.md).
set -euo pipefail
B="$1"; OUT="$2"; shift 2
BETA=0; ID=""; DMG=0
while [ $# -gt 0 ]; do case "$1" in --beta) BETA=1;; --sign) ID="$2"; shift;; --dmg) DMG=1;; *) echo "assemble: unknown option $1"; exit 2;; esac; shift; done
HERE="$(cd "$(dirname "$0")" && pwd)"
ENT="$HERE/../../au_instantiate_probe/EchoJayProbe.entitlements"
if [ "$BETA" = 1 ]; then NAME="EchoJay Wizard Beta"; TGT="ejmap-wizard-beta"; else NAME="EchoJay Wizard"; TGT="ejmap-wizard"; fi
APP_SRC="$B/tools/ejmap/${TGT}_artefacts/RelWithDebInfo/$NAME.app"
[ -d "$APP_SRC" ] || { echo "assemble: no app at $APP_SRC (build the $TGT target)"; exit 2; }
mkdir -p "$OUT"; rm -rf "$OUT/$NAME.app"; ditto "$APP_SRC" "$OUT/$NAME.app"
A="$OUT/$NAME.app"; M="$A/Contents/MacOS"
cp "$B/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app/Contents/MacOS/ejmap" "$M/ejmap"
cp "$B/EchoJayProbe_artefacts/RelWithDebInfo/EchoJayProbe" "$M/EchoJayProbe"
chmod 755 "$M/ejmap" "$M/EchoJayProbe"
R="$A/Contents/Resources"; mkdir -p "$R/mock_db"
# 13 Oct (Kathy): the app icon - the iconutil-built .icns (every size rendered at its own pixels: wizard_app/icon/make_icon.swift) replaces
# JUCE's conversion of the 1024 px PNG (ICON_BIG); the beta carries the BETA ribbon
if [ "$BETA" = 1 ]; then ICNS="$HERE/icon/WizardBeta.icns"; else ICNS="$HERE/icon/Wizard.icns"; fi
[ -f "$ICNS" ] || { echo "assemble: no icon at $ICNS (run wizard_app/icon/make_icon.swift)"; exit 2; }
cp "$ICNS" "$R/Icon.icns"
cp -R "$HERE/mock_db/references" "$R/mock_db/"   # 13 Oct: the mock DB's 290 agreed references (stub only)
if [ "$BETA" = 1 ]; then   # 13 Oct (Kathy): the beta's read-only database - the snapshot of Sean's cert 5 (scripts/beta_db_snapshot.py)
    [ -f "$HERE/beta_db/snapshot.json" ] || { echo "assemble: no beta database at $HERE/beta_db (run scripts/beta_db_snapshot.py)"; exit 2; }
    rm -rf "$R/beta_db"; ditto "$HERE/beta_db" "$R/beta_db"
fi
xattr -dr com.apple.quarantine "$A" 2>/dev/null || true   # a build-tree file can carry a download's flag (12 Oct): leave this Mac clean
if [ -n "$ID" ]; then
    codesign -f -s "$ID" --timestamp --options runtime --entitlements "$ENT" "$M/EchoJayProbe"
    codesign -f -s "$ID" --timestamp --options runtime --entitlements "$ENT" "$M/ejmap"
    codesign -f -s "$ID" --timestamp --options runtime "$A"
    codesign --verify --strict --deep "$A"
    for f in "$M/EchoJayProbe" "$M/ejmap" "$A"; do printf "assemble: %-34s " "$(basename "$f")"; codesign -dv "$f" 2>&1 | grep -E "^TeamIdentifier" ; done
else
    SIG="$(codesign -dv "$M/EchoJayProbe" 2>&1 || true)"
    [[ "$SIG" == *"TeamIdentifier=8BT5F9B887"* ]] || { echo "assemble: the probe is not Developer ID signed - re-sign it first"; exit 3; }
    codesign -f -s - "$M/ejmap"                       # out of its own bundle the engine carries no signature: ad hoc
    codesign -f -s - "$A" 2>/dev/null
    codesign --verify "$A"
fi
echo "assemble: $A"
if [ "$DMG" = 1 ]; then
    STAGE="$(mktemp -d)"; ditto "$A" "$STAGE/$NAME.app"; ln -s /Applications "$STAGE/Applications"
    D="$OUT/$NAME.dmg"; rm -f "$D"
    hdiutil create -volname "$NAME" -srcfolder "$STAGE" -ov -format UDZO "$D" >/dev/null
    rm -rf "$STAGE"
    [ -n "$ID" ] && codesign -f -s "$ID" --timestamp "$D"
    hdiutil verify "$D" >/dev/null && echo "assemble: $D ($(du -h "$D" | cut -f1); drag the app onto Applications)"
fi
