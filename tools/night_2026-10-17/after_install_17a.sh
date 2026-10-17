#!/bin/bash
# Written to run the moment ship_2026-10-17a is installed: the ship folder's own record of WHY it
# was allowed to ship, and the identity proof that the thing in ~/Library is the thing in the folder.
cd /Users/SeanD/echojay-vst || exit 2
set -u
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad
D=ship_2026-10-17a
[ -d "$D" ] || { echo "no $D - nothing installed"; exit 1; }
{
  echo "GATE 27 - ship_2026-10-17a, 17 October 2026"
  echo
  echo "S1  OK   six archives deleted and relinked across build, build-guards, build-release"
  echo "S2  OK   every guard target"
  echo "S3  OK   the four bundles;  S3b OK  archive provenance (content manifest, not mtime)"
  grep -E "^S4 EXIT=|tests passed" "$S/night_gate.log" | sed -e 's/^/S4  /' | head -2
  grep -E "^S5 EXIT=" "$S/night_gate.log" | sed -e 's/^/S5  /' | head -1
  echo "S6  OK   the host rig, live against the local server - the verdict below"
  echo
  grep -E "==== host_rig:" "$S/s6_rerun.log" | sed -e 's/^/     /'
  echo
  echo "Rows that did not run, each with its reason (never counted as passes):"
  grep -E "  SCEN .*SKIP" -A 1 "$S/s6_rerun.log" | sed -e 's/.*host_rig\[[0-9:]*\] //' | grep -vE "^--" | cut -c1-200
  echo
  echo "S6 took seven rounds. Six of them were repairs to the RUNNER, named in the morning report;"
  echo "no product source changed after S1, which is why S1-S5 stand on this tree unaltered."
} > "$D/GREEN.txt"
{
  echo "INSTALLED $(date '+%Y-%m-%d %H:%M %Z')"
  echo
  for P in "EchoJay V2.component/Contents/MacOS/EchoJay V2" \
           "EchoJay Link.component/Contents/MacOS/EchoJay Link"; do
    echo "--- ${P%%/*}"
    dwarfdump --uuid "$D/$P" 2>/dev/null | sed 's/^/  ship   /'
    dwarfdump --uuid "$HOME/Library/Audio/Plug-Ins/Components/$P" 2>/dev/null | sed 's/^/  live   /'
  done
  for P in "EchoJay V2.vst3/Contents/MacOS/EchoJay V2" \
           "EchoJay Link.vst3/Contents/MacOS/EchoJay Link"; do
    echo "--- ${P%%/*}"
    dwarfdump --uuid "$D/$P" 2>/dev/null | sed 's/^/  ship   /'
    dwarfdump --uuid "$HOME/Library/Audio/Plug-Ins/VST3/$P" 2>/dev/null | sed 's/^/  live   /'
  done
} > "$D/INSTALLED.txt"
echo "wrote $D/GREEN.txt and $D/INSTALLED.txt"
ls -la "$D" | head -8
