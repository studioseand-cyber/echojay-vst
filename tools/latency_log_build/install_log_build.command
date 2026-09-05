#!/bin/bash
# Installs the LOG build of EchoJay V2 (AU) over the working one. Quit Logic first.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/EchoJay V2.component"
DEST="$HOME/Library/Audio/Plug-Ins/Components"
LOGDIR="$HOME/echojay-vst/latency-logs"
LOGFILE="$LOGDIR/latency.log"
[ -d "$SRC" ] || { echo "log build not found beside this script: $SRC"; exit 1; }
mkdir -p "$LOGDIR"
if [ -s "$LOGFILE" ]; then
    echo "NOTE: a previous log exists ($(wc -l < "$LOGFILE" | tr -d ' ') lines, last written $(stat -f '%Sm' "$LOGFILE")) - moving it aside as latency.previous.log so this run starts fresh"
    mv "$LOGFILE" "$LOGDIR/latency.previous.log"
else
    echo "no previous log - this run will be the first"
fi
rm -rf "$DEST/EchoJay V2.component"
cp -R "$SRC" "$DEST/"
rm -rf "$HOME/Library/Caches/AudioUnitCache" 2>/dev/null || true
pkill -f AUHostingService 2>/dev/null || true
echo "installed LOG build: $(dwarfdump --uuid "$DEST/EchoJay V2.component/Contents/MacOS/EchoJay V2" | grep arm64 | awk '{print $2}')"
echo "the log will be written to: $LOGFILE   (created when the plugin first reports; nothing to send)"
echo "now: open Logic, press play four or five times (once after jumping to a marker), quit Logic, then run restore_working_build.command"
