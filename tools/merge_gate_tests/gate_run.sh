#!/bin/bash
# THE GATE RUN LIST (6 Sep 2026, from the gate audit): everything that exists and is runnable
# RUNS here, each in its own private state root (ISOLATION ruling - see EJStateRoot.h).
# Usage: tools/merge_gate_tests/gate_run.sh [build-dir]   (default build-release; gate_build.sh first)
# One line per run in $OUT/summary.txt; exit 1 if any run fails or is refused.
# EXCLUDED (with reason, kept in this list so the exclusion is visible):
#   bridged_readback_test  - 3/1 known machine-state fail (WaveShell 15.0 universal on this Mac), not code
#   pitch_click_test       - an instrument: needs input material and reports density, no verdict
#   tools/pitch_*/ (21)    - instruments/probes, no verdict, no build_and_run.sh
#   reinstall_v2_test      - tests tools/reinstall-v2.sh, which is never run on this project
#   link_provenance_negctl - a NEGATIVE CONTROL against the pre-fix build; run by hand when the fix changes
cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
A=${1:-build-release}; MG=tools/merge_gate_tests
OUT=${GATE_OUT:-$(mktemp -d /tmp/echojay-gate.XXXXXX)}; mkdir -p $OUT/bin; SUM=$OUT/summary.txt; : > $SUM
fails=0
run() { local n=$1 wd=$2; shift 2; local root; root=$(mktemp -d /tmp/echojay-iso.XXXXXX); local t0=$(date +%s)
  ( ECHOJAY_STATE_HOME=$root "$@" > $OUT/$n.log 2>&1; echo "exit=$?" >> $OUT/$n.log ) & local P=$!
  for i in $(seq 1 $wd); do kill -0 $P 2>/dev/null || break; sleep 1; done
  if kill -0 $P 2>/dev/null; then pkill -P $P 2>/dev/null; kill $P 2>/dev/null; echo "WATCHDOG" >> $OUT/$n.log; echo "exit=124" >> $OUT/$n.log; fi
  local ex=$(grep -o '^exit=[0-9]*' $OUT/$n.log | tail -1); [ "$ex" = "exit=0" ] || fails=$((fails+1))
  printf '%-26s %-9s %4ss\n' "$n" "$ex" "$(( $(date +%s) - t0 ))" | tee -a $SUM; }
for h in link_two_same_name_test link_capacity_test; do
  ECHOJAY_STATE_HOME=$(mktemp -d /tmp/echojay-iso.XXXXXX) $MG/compile_link_harness.sh $MG/$h.cpp $OUT/bin/$h > $OUT/compile_$h.log 2>&1 || { echo "$h COMPILE FAILED" | tee -a $SUM; fails=$((fails+1)); }
done
B=$OUT/bin
run link2_default 240 $B/link_two_same_name_test;   run link2_l567 240 $B/link_two_same_name_test l567
run link2_race20 600 $B/link_two_same_name_test race20; run link2_l6x20 600 $B/link_two_same_name_test l6x20
run link2_storm 300 $B/link_two_same_name_test storm
run cap_default 300 $B/link_capacity_test; run cap_p20 600 $B/link_capacity_test p20; run cap_churn20 600 $B/link_capacity_test churn20
run numbering50 300 $MG/build_and_run.sh $MG/link_untitled_numbering_test.cpp 50
run block_cost 300 $MG/build_and_run.sh $MG/link_block_cost_test.cpp
run levels_payload 300 $MG/build_and_run.sh $MG/link_levels_payload_test.cpp 45
run v9_dialwrites 300 $MG/build_and_run.sh $MG/v9_dialwrites_restore_test.cpp
run v9_rack 300 $MG/build_and_run.sh $MG/v9_rack_restore_test.cpp
for t in EchoJayPitchModeTest EchoJayPitchHostTest EchoJayBuiltinRegistryTest; do
  bin=$(find $A -name $t -type f -perm +111 | head -1); [ -n "$bin" ] && run $t 600 $bin || { echo "$t: not built" | tee -a $SUM; fails=$((fails+1)); }; done
run load_V2   120 $MG/build_and_run.sh $MG/plugin_load_test.mm "$A/EchoJay_artefacts/Release/VST3/EchoJay V2.vst3" "$A/EchoJay_artefacts/Release/AU/EchoJay V2.component"
run load_Link 120 $MG/build_and_run.sh $MG/plugin_load_test.mm "$A/EchoJayLink_artefacts/Release/VST3/EchoJay Link.vst3" "$A/EchoJayLink_artefacts/Release/AU/EchoJay Link.component"
run block_cost256 300 $MG/build_and_run.sh $MG/link_block_cost_test.cpp 256
for n in art_parity_test borrowhost_test chainguidance_test dashboard_test dashweb_test latency_impulse_test linkmixer_test linksync_test mapfps_test meterbench paramapply_test pitch_ab_test racklock_test ringseek_test settings_snapshot state_match_test structplan_test tabstrip_test tier_identity_gate workspace_roundtrip_test; do
  run $n 400 tools/$n/build_and_run.sh; done
echo "gate run: $fails failing/refused  (logs: $OUT)" | tee -a $SUM; [ $fails -eq 0 ]
