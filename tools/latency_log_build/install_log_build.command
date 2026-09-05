#!/bin/bash
# Installs the LOG build of EchoJay V2 (AU) over the working one. Quit Logic first.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/EchoJay V2.component"
DEST="$HOME/Library/Audio/Plug-Ins/Components"
[ -d "$SRC" ] || { echo "log build not found beside this script: $SRC"; exit 1; }
rm -rf "$DEST/EchoJay V2.component"
cp -R "$SRC" "$DEST/"
rm -rf "$HOME/Library/Caches/AudioUnitCache" 2>/dev/null || true
pkill -f AUHostingService 2>/dev/null || true
echo "installed LOG build: $(dwarfdump --uuid "$DEST/EchoJay V2.component/Contents/MacOS/EchoJay V2" | grep arm64 | awk '{print $2}')"
echo "log file: $HOME/Library/Logs/EchoJay/latency.log  (created when the plugin first reports)"
echo "now open Logic, press play a few times, quit Logic, then run restore_working_build.command"
