#!/bin/bash
# =============================================================================
#  THE ONE-MINUTE SAME-PROCESS PROBE (his 26 Oct ruling, §1 of the design)
# =============================================================================
#  "Ableton and Cubase probes: write the one-minute steps for me and I'll run
#   them later."
#
#  Answers one question about whatever host is running right now: does the V2
#  run in the SAME process as the Link? That is the whole of what decides
#  whether the live view can show the real plugin (echojay::embedAllowed) or
#  has to use the copy-leads view.
#
#  Usage:  bash tools/same_process_probe.sh [minutes]      (default 10)
#  Reads only the log. Changes nothing, loads nothing, needs no host closed.
# =============================================================================
set -u
MINS="${1:-10}"
LOG_DIR="$HOME/Library/EchoJay/logs"
SINCE=$(date -v-"${MINS}"M '+%Y-%m-%d %H:%M:%S')
echo "######## same-process probe: lines since $SINCE"
echo

# Newest log first, oldest last, so a session that rolled over is still covered.
FILES=$(ls -t "$LOG_DIR"/echojay-*.log 2>/dev/null)
[ -z "$FILES" ] && { echo "No EchoJay logs in $LOG_DIR - has the plugin been loaded at all?"; exit 2; }

TMP=$(mktemp)
# shellcheck disable=SC2086
awk -v since="$SINCE" '
  /^20[0-9][0-9]-/ {
    stamp = substr($0, 1, 19)
    gsub(/  +/, " ", stamp)
    keep = (stamp >= since)
  }
  keep { print }
' $FILES > "$TMP"

echo "pid       V2-only lines   Link-only lines   verdict"
echo "-------   -------------   ---------------   -------------------------------------"
PIDS=$(grep -oE '^\[?[0-9-]{10} [0-9:]{8} +\[[0-9]+\]' "$TMP" | grep -oE '\[[0-9]+\]' | tr -d '[]' | sort -u)
if [ -z "$PIDS" ]; then
  echo "(none)    -               -                 no line in the window carries a pid."
  echo
  echo "A build older than 12 Oct 2026 does not log its pid. Check the host's own process"
  echo "instead:   log show --predicate 'eventMessage CONTAINS \"EchoJay Link)\"' --last ${MINS}m --style compact | head"
  rm -f "$TMP"; exit 0
fi
SHARED=0
for p in $PIDS; do
  v2=$(grep -c "\[$p\].*\(EJPane(main)\|EJPanel\|EJChat\|EJScan\)" "$TMP")
  lk=$(grep -c "\[$p\].*\(EJLinkMeter\|EJPane(link)\|EJLinkState: registry snapshot\)" "$TMP")
  if [ "$v2" -gt 0 ] && [ "$lk" -gt 0 ]; then
    verdict="BOTH in this process - the real-editor road is available"; SHARED=1
  elif [ "$v2" -gt 0 ]; then verdict="a V2 only"
  elif [ "$lk" -gt 0 ]; then verdict="a Link only"
  else verdict="neither (housekeeping lines only)"; fi
  printf "%-9s %-15s %-17s %s\n" "$p" "$v2" "$lk" "$verdict"
done
echo
if [ "$SHARED" = "1" ]; then
  echo "VERDICT: this host puts the V2 and the Link in ONE process."
else
  echo "VERDICT: no single process held both. Either they are in separate processes (so this host"
  echo "         takes the copy-leads road), or one of the two was never loaded in the window."
  echo "         Load one of each, open both windows, then run this again."
fi
rm -f "$TMP"
