#!/bin/bash
# hosted_exit_probe: the evidence, in both directions, for ONE rule -
#
#   a host tool of ours that hosts a third-party AU must leave the process by _Exit, never by returning from
#   main, because the plugin's own threads are still running and __cxa_finalize destroys the statics under them.
#
# known-bad  (--door return)    : expected to DIE, in the plugin's own thread, with the main thread in exit()
# known-good (--door fastexit)  : expected to leave 0 every time
#
# RED if either direction fails to behave: a run where the known-bad door survives proves nothing, and is
# reported as "no evidence" rather than quietly passing (tools/VERIFIER rule, 2 Oct).
#
#   tools/hosted_exit_probe/build_and_run.sh [trials]      (default 3)
#
# No full scan, ever: each subject is named by identifier. None of the three is PACE-wrapped and the probe
# refuses one anyway.
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT="$PWD"
TRIALS="${1:-3}"
OUT="${OUT:-/tmp/hosted_exit_probe}"
LOGDIR="${LOGDIR:-/tmp/hosted_exit_probe_logs}"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

echo "==== hosted_exit_probe: building"
LIB="${LIB:-$ROOT/build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a}"
[ -f "$LIB" ] || { echo "no SharedCode lib at: $LIB"; exit 1; }
python3 - "$OUT" "$LIB" <<'PYEOF'
import json, sys, shlex, subprocess, os
out_bin, lib = sys.argv[1], sys.argv[2]
cc = json.load(open('build/compile_commands.json'))
entry = [e for e in cc if e['file'].endswith('Source/ChainHost.cpp') and 'CMakeFiles/EchoJay.dir' in e['command']][0]
args = shlex.split(entry['command'])
out, skip = [], False
for a in args[1:]:
    if skip: skip = False; continue
    if a in ('-c', '-o'): skip = (a == '-o'); continue
    if a.endswith('ChainHost.cpp') or a.endswith('.o'): continue
    out.append(a)
# Only the slices this lib actually has (26 Oct: the rig linked an arm64-only archive into a universal build).
archs = subprocess.run(['lipo','-archs',lib], capture_output=True, text=True).stdout.split()
keep, i = [], 0
while i < len(out):
    if out[i] == '-arch':
        if i + 1 < len(out) and out[i+1] in archs: keep += [out[i], out[i+1]]
        i += 2; continue
    keep.append(out[i]); i += 1
cmd = (['clang++'] + keep + ['-I', os.path.abspath('Source'),
        'tools/hosted_exit_probe/probe.cpp', lib,
        '-framework','Cocoa','-framework','CoreAudio','-framework','CoreMIDI',
        '-framework','AudioToolbox','-framework','Accelerate','-framework','QuartzCore',
        '-framework','IOKit','-framework','Security','-framework','WebKit',
        '-framework','Metal','-framework','MetalKit','-framework','CoreAudioKit',
        '-framework','UniformTypeIdentifiers','-framework','AVFoundation',
        '-framework','CoreMedia','-framework','AVKit','-framework','OpenGL','-lcurl',
        '-o', out_bin])
r = subprocess.run(cmd, capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-4000:]); sys.exit(1)
print("BUILT:", out_bin)
PYEOF
[ $? -eq 0 ] || exit 1

# name | identifier
# MCompressor IS NOT HERE, and that is a finding of its own (27 Oct): /Library/Audio/Plug-Ins/Components/
# MCompressor.component/Contents/__Pace_Eden.bundle - it is PACE-wrapped, so an unsigned harness process must
# never load it, and this probe refuses it. It was named as a screened subject in host_rig's subject list
# overnight; that screening was wrong and host_rig now refuses it the same way.
SUBJECTS=(
  "ValhallaDelay|AudioUnit:Effects/aufx,dLay,oDin"
  "TDR SlickEQ M|AudioUnit:Effects/aufx,Td6a,Tdrl"
  "Pro-C 3|AudioUnit:Effects/aumf,FC3p,FabF"
  "AUMatrixReverb|AudioUnit:Effects/aufx,mrev,appl"
  "AUDelay|AudioUnit:Effects/aufx,dely,appl"
)

# EVERY DOOR DOES THE SAME WORK FIRST, and that work includes the plugin's OWN EDITOR, three opens and
# closes of it. Without an editor neither door reproduced anything: a plugin's timer thread only exists once
# something inside it starts a juce::Timer, and its editor is the usual thing that does. With it, the
# known-bad door died first try.
WORK="--editor 3"

fails=0
declare -a SUMMARY
for s in "${SUBJECTS[@]}"; do
  name="${s%%|*}"; ident="${s##*|}"
  for door in return fastexit; do
    died=0; ok=0; codes=""
    for t in $(seq 1 "$TRIALS"); do
      log="$LOGDIR/$(echo "$name" | tr ' ' '_')-$door-$t.log"
      # No coreutils `timeout` on this Mac: a watchdog that kills the probe if a plugin hangs on load
      # (Fresh Air never returned from instantiation, 26 Oct) and reports 124, as timeout would.
      "$OUT" "$name" "$ident" 0 --door "$door" $WORK > "$log" 2>&1 &
      pid=$!
      ( sleep 60; kill -9 "$pid" 2>/dev/null ) & wd=$!
      wait "$pid"; rc=$?
      kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null
      if [ "$rc" -eq 137 ] && ! grep -q "RESULT:" "$log"; then rc=124; fi
      codes="$codes $rc"
      # 139 = SIGSEGV, 134 = SIGABRT, 133 = SIGTRAP: any signal death is "died".
      if [ "$rc" -ge 128 ]; then died=$((died+1)); elif [ "$rc" -eq 0 ]; then ok=$((ok+1)); fi
      grep -q "INSTANTIATE: OK" "$log" || { echo "  note: $name did not instantiate on trial $t (door $door)"; }
    done
    echo "  $name  door=$door  trials=$TRIALS  died=$died  clean=$ok  exit codes:$codes"
    SUMMARY+=("$name|$door|$died|$ok")
  done
done

echo
echo "==== the two directions"
for name_only in "ValhallaDelay" "TDR SlickEQ M" "Pro-C 3" "AUMatrixReverb" "AUDelay"; do
  bad=""; good=""
  for r in "${SUMMARY[@]}"; do
    IFS='|' read -r n d died ok <<< "$r"
    [ "$n" = "$name_only" ] && [ "$d" = "return" ]   && bad="$died/$TRIALS died"
    [ "$n" = "$name_only" ] && [ "$d" = "fastexit" ] && good="$ok/$TRIALS clean"
  done
  echo "  $name_only: returning from main -> $bad ; _Exit -> $good"
done

# The assertions. ValhallaDelay is the SUBJECT: it is the plugin whose exit-time race was observed eleven times,
# so it is the one that must show both directions. The other three are context, and only their good door is
# asserted - a plugin that survives static destruction is not a fault.
vd_bad=0; vd_good=0
for r in "${SUMMARY[@]}"; do
  IFS='|' read -r n d died ok <<< "$r"
  [ "$n" = "ValhallaDelay" ] && [ "$d" = "return" ]   && vd_bad="$died"
  [ "$n" = "ValhallaDelay" ] && [ "$d" = "fastexit" ] && vd_good="$ok"
done
echo
if [ "$vd_bad" -lt 1 ]; then
  echo "  FAIL known-bad door survived: returning from main with ValhallaDelay alive did not die in $TRIALS trials - THIS RUN IS NO EVIDENCE, not a pass"
  fails=$((fails+1))
else
  echo "  ok   known-bad: returning from main with ValhallaDelay alive dies ($vd_bad of $TRIALS)"
fi
if [ "$vd_good" -ne "$TRIALS" ]; then
  echo "  FAIL known-good door is not clean: _Exit with ValhallaDelay alive left $vd_good of $TRIALS cleanly"
  fails=$((fails+1))
else
  echo "  ok   known-good: _Exit with ValhallaDelay alive leaves cleanly ($TRIALS of $TRIALS)"
fi
for r in "${SUMMARY[@]}"; do
  IFS='|' read -r n d died ok <<< "$r"
  if [ "$d" = "fastexit" ] && [ "$n" != "ValhallaDelay" ] && [ "$ok" -ne "$TRIALS" ]; then
    echo "  FAIL _Exit was not clean for $n ($ok of $TRIALS)"; fails=$((fails+1))
  fi
done

echo
if [ "$fails" -eq 0 ]; then echo "==== hosted_exit_probe: GREEN (logs in $LOGDIR)"; exit 0; fi
echo "==== hosted_exit_probe: RED ($fails) (logs in $LOGDIR)"; exit 1
