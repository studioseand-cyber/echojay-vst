#!/bin/bash
# Place ship_2026-10-16b from build-guards and install it. Run ONLY after a green gate.
set -u
cd /Users/SeanD/echojay-vst || exit 2
if pgrep -x "Logic Pro" > /dev/null; then
  echo "REFUSING: Logic Pro is running - the install would be replaced under a live host."
  exit 1
fi
echo "######## placing ship_2026-10-16b from build-guards ########"
bash tools/place_ship.sh ship_2026-10-16b build-guards || { echo "PLACE FAILED"; exit 1; }
ls -la ship_2026-10-16b | head
echo "######## installing ########"
bash tools/install_local.sh build-guards || { echo "INSTALL FAILED"; exit 1; }
echo "######## killing AUHostingService (twice, the standing rule) ########"
killall -9 AUHostingService 2>/dev/null; sleep 1; killall -9 AUHostingService 2>/dev/null; true
echo "######## identities: ship folder vs ~/Library, both slices ########"
for P in "EchoJay V2.component/Contents/MacOS/EchoJay V2" \
         "EchoJay Link.component/Contents/MacOS/EchoJay Link"; do
  S="ship_2026-10-16b/${P%%/*}"; B="$HOME/Library/Audio/Plug-Ins/Components/${P%%/*}"
  echo "--- ${P%%/*}"
  dwarfdump --uuid "ship_2026-10-16b/$P" 2>/dev/null | sed 's/^/  ship   /'
  dwarfdump --uuid "$HOME/Library/Audio/Plug-Ins/Components/$P" 2>/dev/null | sed 's/^/  live   /'
done
for P in "EchoJay V2.vst3/Contents/MacOS/EchoJay V2" \
         "EchoJay Link.vst3/Contents/MacOS/EchoJay Link"; do
  echo "--- ${P%%/*}"
  dwarfdump --uuid "ship_2026-10-16b/$P" 2>/dev/null | sed 's/^/  ship   /'
  dwarfdump --uuid "$HOME/Library/Audio/Plug-Ins/VST3/$P" 2>/dev/null | sed 's/^/  live   /'
done
echo "######## done ########"
