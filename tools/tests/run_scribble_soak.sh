#!/bin/bash
# run_scribble_soak.sh — N clean scribble runs, or RED (26 Sep 2026 ruling: "guard on the loudness_loop_guard
# scribble leg at N=6 clean").
#
# WHY N RUNS AND NOT ONE. The defect this is the acceptance for was FLAKY: the graph's async rebuild walked a map
# the audio thread had corrupted during teardown, and it crashed in about half of runs, always after the last
# assertion. A single green run of a 50 % failure is not evidence of anything - it is one coin toss - and a single
# RED run was what led to a runner change being made against scheduling noise earlier the same day. So the
# acceptance is N consecutive clean runs, the count is printed, and any failure names the run and its signal.
#
# Usage: run_scribble_soak.sh <binary> [N]        (default N = 6)
set -u
BIN="$1"; N="${2:-6}"
BASE="${EJ_GUARD_HOME_BASE:-/tmp}"; mkdir -p "$BASE" 2>/dev/null
HERE="$(cd "$(dirname "$0")" && pwd)"
green=0; red=0
for n in $(seq 1 "$N"); do
  OUT="$BASE/soak-$(basename "$BIN")-$n.out"
  if bash "$HERE/run_guard.sh" "$BIN" --scribble > "$OUT" 2>&1; then
    green=$((green+1)); echo "  run $n/$N: GREEN  ($(grep -c '^  ok' "$OUT") assertion(s))"
  else
    red=$((red+1))
    echo "  run $n/$N: RED  ($(grep -c '^  ok' "$OUT") assertion(s) passed first)"
    grep -aE "Abort trap|Segmentation fault|Bus error|Trace/BPT|FAIL |assertion\(s\) failed|BOTH LEGS" "$OUT" | tail -4 | sed 's/^/      /'
  fi
done
echo "==== scribble soak: $green/$N GREEN, $red/$N RED ===="
[ "$red" -eq 0 ] || exit 1
