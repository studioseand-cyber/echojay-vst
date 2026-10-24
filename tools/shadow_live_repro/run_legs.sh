#!/usr/bin/env bash
#
# run_legs.sh — the supervised bench repro of his 12:14 live-view crash.
#
# ONE-OFF EXCEPTION TO THE RIG RULE. The rig never loads iLok or PACE plugins, because a licence
# prompt or a PACE bootstrap failure puts a window on Sean's desktop and has crashed this Mac. He
# granted an exception for this plugin, with him at the keyboard, so this script:
#   * refuses to run with a host open (Logic or Pro Tools), so nothing competes for the licence;
#   * SIGNS the harness with his Developer ID and the probe's entitlements first, because an
#     unsigned host cannot bootstrap PACE at all (EJPaceCheck.h, 2 Oct ruling);
#   * runs each leg as its OWN process, so a crash in one leg still leaves the rest to run, and
#     reports the signal and how far it got;
#   * never writes EchoJay state and never installs anything.
#
# Usage:  tools/shadow_live_repro/run_legs.sh [seconds-per-leg] [leg ...]
#
set -uo pipefail
cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"

SECS="${1:-60}"
shift || true
LEGS=("$@")
if [ ${#LEGS[@]} -eq 0 ]; then LEGS=(noeditor product keyfed bigblock playhead); fi

BIN="build-guards/EchoJayShadowRepro_artefacts/Release/EchoJayShadowRepro"
[ -x "$BIN" ] || BIN="build-guards/EchoJayShadowRepro_artefacts/EchoJayShadowRepro"
ID="Developer ID Application: Sean Donoghue (8BT5F9B887)"
ENTS="tools/au_instantiate_probe/EchoJayProbe.entitlements"
OUT="${EJ_REPRO_OUT:-${TMPDIR:-/tmp}/ej_shadow_repro}"

if pgrep -x "Logic Pro" > /dev/null || pgrep -x "Pro Tools" > /dev/null; then
    echo "REFUSED: a host is open. Close Logic and Pro Tools first - this loads the same licence."
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "REFUSED: no harness at $BIN"
    echo "         cmake --build build-guards --target EchoJayShadowRepro -j 4"
    exit 1
fi

# SIGNED, every run: the binary is relinked by every build and a signature does not survive that.
codesign -f -s "$ID" --timestamp --options runtime --entitlements "$ENTS" "$BIN" \
    || { echo "REFUSED: could not sign $BIN"; exit 1; }
codesign -dv --entitlements - "$BIN" 2>&1 | grep -E "Authority|allow-unsigned|disable-library" | sed 's/^/  /'

mkdir -p "$OUT"
echo
echo "######## $(date '+%H:%M:%S') the legs: ${LEGS[*]}  (${SECS}s each) ########"
for LEG in "${LEGS[@]}"; do
    LOG="$OUT/repro_${LEG}.log"
    T0=$(date +%s)
    echo
    echo "---- $LEG ----"
    "$BIN" "$LEG" "$SECS" > "$LOG" 2>&1
    RC=$?
    T=$(( $(date +%s) - T0 ))
    LAST=$(grep -E "^\[$LEG\] (t=|SURVIVED|REFUSED)" "$LOG" | tail -1)
    if [ $RC -eq 0 ]; then
        echo "  SURVIVED  ${T}s  exit=0   $LAST"
    elif [ $RC -gt 128 ]; then
        echo "  CRASHED   ${T}s  signal $(( RC - 128 ))   $LAST"
        echo "            the last lines:"
        tail -4 "$LOG" | sed 's/^/            /'
    else
        echo "  stopped   ${T}s  exit=$RC  $LAST"
    fi
done
echo
echo "logs: $OUT"
