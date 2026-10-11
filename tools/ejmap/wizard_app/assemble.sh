#!/bin/bash
# Assemble "EchoJay Wizard.app" with its engine: the app from the build, `ejmap` (the wizard engine) and the Developer ID signed
# EchoJayProbe beside it in Contents/MacOS, then the bundle re-sealed ad hoc WITHOUT --deep (the probe keeps its own signature).
#   tools/ejmap/wizard_app/assemble.sh <build dir> <output folder>
# Notarisation / Developer ID for the app itself: WHATS_LEFT.md (not done here).
set -euo pipefail
B="$1"; OUT="$2"
APP_SRC="$B/tools/ejmap/ejmap-wizard_artefacts/RelWithDebInfo/EchoJay Wizard.app"
mkdir -p "$OUT"; rm -rf "$OUT/EchoJay Wizard.app"; cp -R "$APP_SRC" "$OUT/"
M="$OUT/EchoJay Wizard.app/Contents/MacOS"
cp "$B/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app/Contents/MacOS/ejmap" "$M/ejmap"
cp "$B/EchoJayProbe_artefacts/RelWithDebInfo/EchoJayProbe" "$M/EchoJayProbe"
SIG="$(codesign -dv "$M/EchoJayProbe" 2>&1 || true)"
[[ "$SIG" == *"TeamIdentifier=8BT5F9B887"* ]] || { echo "assemble: the probe is not Developer ID signed - re-sign it first"; exit 3; }
codesign -f -s - "$M/ejmap"                       # out of its own bundle the engine carries no signature: ad hoc
codesign -f -s - "$OUT/EchoJay Wizard.app" 2>/dev/null
codesign --verify "$OUT/EchoJay Wizard.app" && echo "assemble: $OUT/EchoJay Wizard.app (engine + probe inside; probe signed by team 8BT5F9B887)"
