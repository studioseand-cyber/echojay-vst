#!/bin/bash
# Puts the WORKING build back (the one tools/install_local.sh build-release installs). Quit Logic first.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/working/EchoJay V2.component"
DEST="$HOME/Library/Audio/Plug-Ins/Components"
[ -d "$SRC" ] || { echo "working build copy not found: $SRC"; exit 1; }
rm -rf "$DEST/EchoJay V2.component"
cp -R "$SRC" "$DEST/"
rm -rf "$HOME/Library/Caches/AudioUnitCache" 2>/dev/null || true
pkill -f AUHostingService 2>/dev/null || true
echo "restored WORKING build: $(dwarfdump --uuid "$DEST/EchoJay V2.component/Contents/MacOS/EchoJay V2" | grep arm64 | awk '{print $2}')"
