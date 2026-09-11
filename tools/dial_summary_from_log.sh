#!/bin/bash
# Pull Build H's dial-summary lines from a live Pro Tools session and report, per slot:
# requested / applied / omitted, and whether the map came from DISK or a FETCH. Run after Sean
# builds a chain on Build H. Usage: tools/dial_summary_from_log.sh [since 'YYYY-MM-DD HH:MM:SS']
set -u
SINCE="${1:-$(date -v-30M '+%Y-%m-%d %H:%M:%S')}"
NOW="$(date '+%Y-%m-%d %H:%M:%S')"
echo "== EchoJay dial trail, $SINCE .. $NOW =="
log show --start "$SINCE" --end "$NOW" \
  --predicate 'process == "Pro Tools" AND (eventMessage CONTAINS "EJDialSummary" OR eventMessage CONTAINS "EJParamApply" OR eventMessage CONTAINS "NO MAP" OR eventMessage CONTAINS "EJParamMaps: fetch" OR eventMessage CONTAINS "EJParamMaps: stored" OR eventMessage CONTAINS "EJParamMaps: cache loaded" OR eventMessage CONTAINS "EJDialable")' \
  --style compact 2>/dev/null \
  | grep -v '^Timestamp\|^Filtering' \
  | sed -E 's/^[^ ]+ ([0-9:.]+) [^ ]+ Pro Tools\[[0-9]+:[0-9a-f]+\] \(EchoJay ([A-Za-z0-9]+)\) /\1 [\2] /' \
  | grep -ivE 'EXPECTED ORDERING'
echo
echo "READING IT:"
echo "  EJParamMaps: cache loaded, N map(s)  -> maps present on DISK at load"
echo "  EJParamMaps: fetch ... / stored ...  -> the FETCH path fired (map was NOT cached; this is the new wiring)"
echo "  EJDial: ... NO MAP ... fetch_wired=y in_flight=y -> a fetch was requested (good); fetch_wired=n -> the OLD defect"
echo "  EJDialSummary: slot i ... requested=R applied=A ... status=applied|noMap|mapNoCoverage|writesRejected"
echo "  A borrowed-build summary appearing AT ALL is the observability that used to be silent."
