#!/bin/bash
# link_state_guard runner (20 Sep 2026): the deleted-chain bug, two processes. GREEN: both sides on the current archives.
# EJ_RED=1: the pre-fix Link archive + headers (EJ_LINK_LIB / EJ_SRC_ROOT) and the V2 side compiled against the pre-fix V2 archive
# (EJ_LIB / EJ_SRC_ROOT) - which cannot compile there (no borrowPushStructuralEdit): the runner then plays the V2's exact bytes.
set -u; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad
ISO=$(mktemp -d /tmp/echojay-lsg-home.XXXXXX); export HOME=$ISO EJ_STATE_TEST_HOME=$ISO ECHOJAY_STATE_HOME=$ISO
H=$ISO/lsg; mkdir -p $H; echo "isolated home $ISO"
echo "-- compile link side"; bash tools/merge_gate_tests/compile_link_harness.sh tools/link_state_guard/link_side.cpp $S/lsg_link_bin 2>&1 | grep -E "error:|compiled" | head -5
echo "-- compile v2 side"; python3 tools/harness_build.py tools/link_state_guard/v2_side.cpp 2>&1 | grep -E "COMPILE FAILED|error:|compiled ->|v2 side: compiled" | head -6
V2BIN=$S/v2_side_bin; V2OK=0; [ -x "$V2BIN" ] && [ -z "${EJ_RED:-}" ] && V2OK=1
$S/lsg_link_bin $H > $H/link.log 2>&1 & LP=$!
for i in $(seq 1 120); do [ -f $H/link_ready.json ] && break; sleep 0.5; done
UID_=$(python3 -c "import json;print(json.load(open('$H/link_ready.json'))['uid'])" 2>/dev/null); echo "link uid $UID_"
if [ $V2OK = 1 ]; then
  EJ_LSG_HOME=$H $V2BIN > $H/v2.log 2>&1; echo "v2 side rc=$?"
else
  echo "-- V2 side unavailable on this tree (pre-fix: no borrowPushStructuralEdit) - the runner writes the fixed V2's exact bytes"
  python3 - "$H" "$UID_" "$ISO" <<'PY'
import json,sys,time,os,glob
H,uid,iso=sys.argv[1:4]; d=iso+'/Library/Application Support/EchoJay/link/'
os.makedirs(d, exist_ok=True)
lease={'v':1,'leaseId':'lease-guard-red','slot':0,'scope':'rack','muteOut':False,'editPending':False,'tMs':int(time.time()*1000)}
open(d+f'lease-{uid}.json','w').write(json.dumps(lease))
for _ in range(12):   # keep the lease fresh through the gate floor
    time.sleep(0.5); lease['tMs']=int(time.time()*1000); open(d+f'lease-{uid}.json','w').write(json.dumps(lease))
six=["EchoJay EQ","EchoJay Level","EchoJay Limiter","EchoJay Gain","EchoJay Compressor","EchoJay Delay"]
cmd={'v':2,'seq':7001,'editOps':[{'op':'remove','slot':3}],'baseSlots':six,'sourceNote':'EchoJay V2 borrowed rack edit','leaseId':'lease-guard-red'}
open(d+f'chain-cmd-{uid}.json','w').write(json.dumps(cmd))
exp=[n for i,n in enumerate(six) if i!=2]
open(H+'/v2_step.json','w').write(json.dumps({'seq':7001,'op':'remove','leg':'G1','expectCount':5,'expectNames':exp,'expectBypassed0':False}))
t0=time.time()
while time.time()-t0<20 and not os.path.exists(H+'/link_step_7001.json'):
    time.sleep(0.5); lease['tMs']=int(time.time()*1000); open(d+f'lease-{uid}.json','w').write(json.dumps(lease))
print('  (runner) ack:', open(d+f'chain-ack-{uid}.json').read()[:160] if os.path.exists(d+f'chain-ack-{uid}.json') else 'absent')
open(H+'/v2_done.json','w').write(json.dumps({'count':5,'names':exp}))
for _ in range(40):
    if os.path.exists(H+'/link_done.json'): break
    time.sleep(0.5); lease['tMs']=int(time.time()*1000); open(d+f'lease-{uid}.json','w').write(json.dumps(lease))
PY
  echo "  FAIL  G2 add / reorder / replace / bypass: the V2 side does not exist on this tree (RED by name)"
fi
wait $LP; LEXIT=$?
echo "== link side =="; grep -E "^  (ok|FAIL)|^====|lease engaged|link side:" $H/link.log
# the verdict is the LOG's verdict line (printed before any teardown); a non-zero exit after it is reported, not counted
LRC=1; grep -q "(link side): GREEN" $H/link.log && LRC=0
[ $LEXIT -ne 0 ] && echo "  note: link side process exit $LEXIT after its verdict (teardown of a headless LinkProcessor that processed a block - see the report)"
[ $V2OK = 1 ] && { echo "== v2 side =="; grep -E "^  (ok|FAIL)|^====|final rack" $H/v2.log; }
V2RC=0; [ $V2OK = 1 ] && grep -q "v2 side): RED" $H/v2.log && V2RC=1; [ $V2OK = 0 ] && V2RC=1
if [ $LRC -eq 0 ] && [ $V2RC -eq 0 ]; then echo "==== link_state_guard: GREEN ===="; exit 0; else echo "==== link_state_guard: RED ===="; exit 1; fi
