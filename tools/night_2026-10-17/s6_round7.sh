#!/bin/bash
# S6 round 7 - every runner fix of the night in: usable-Link resolution and re-resolution, A2's
# stale-Link skip, C7's single compressor, C2-live and E10-live attributed to the server, B3-live
# answering with a real chip, I4 attribution, C2's settling bound.
# Installs ship_2026-10-17a only if S4/S5 passed in the gate run AND this S6 is fully green.
cd /Users/SeanD/echojay-vst || exit 2
set -u
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad
while pgrep -f "build_and_run.sh" > /dev/null; do sleep 10; done
S4OK=$(grep -c "^S4 EXIT=0" "$S/night_gate.log"); S5OK=$(grep -c "^S5 EXIT=0" "$S/night_gate.log")
if [ "$S4OK" != "1" ] || [ "$S5OK" != "1" ]; then echo "REFUSING: S4/S5 not both green"; exit 1; fi
echo "######## S6 round 7 ########"
bash "$S/s6_rerun_17a.sh" 1
# BOTH HALVES, COUNTED (17 Oct 2026). This read `grep -q "host_rig: GREEN"` and S6 prints TWO
# verdict lines - the live rig and the mock. Round 7's live half was green and its mock half was RED,
# and this check saw the first line and installed. A verifier that can only answer yes is not a
# verifier; this one counts the verdicts and requires every one of them to be green.
VERDICTS=$(grep -c "==== host_rig: " "$S/s6_rerun.log")
GREENS=$(grep -c "==== host_rig: GREEN" "$S/s6_rerun.log")
echo "S6 verdict lines: $VERDICTS, of which green: $GREENS"
if [ "$VERDICTS" -lt 2 ] || [ "$GREENS" != "$VERDICTS" ]; then
  echo "######## S6 ROUND 4 IS NOT GREEN - NOTHING IS INSTALLED ########"
  grep -E "(SCEN|ROW) .*FAIL" -A 1 "$S/s6_rerun.log" | sed -e 's/.*host_rig\[[0-9:]*\] //' | cut -c1-220
  exit 1
fi
echo "######## S6 GREEN - placing and installing ship_2026-10-17a ########"
sed -e 's/ship_2026-10-16b/ship_2026-10-17a/g' tools/ship_and_install_2026-10-16b.sh > "$S/ship_17a.sh"
bash "$S/ship_17a.sh"
echo "######## ROUND 4 DONE ########"
