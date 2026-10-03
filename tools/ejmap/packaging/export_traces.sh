#!/bin/bash
# Pack one certified product's traces from this repo into a zip that unpacks over another Mac's ~/Library/ejmap/cert,
# so the tone-check-only mode (--cert-tonecheck-all) can re-derive and tone-check it there without a sweep.
# The binary's section 11 guard refuses the check if the installed version differs from the record's.
#   tools/ejmap/packaging/export_traces.sh <identity stem> <traces dir> <out.zip>
#   e.g. tools/ejmap/packaging/export_traces.sh AudioUnit_517f614e_2.5.62 tools/ejmap/cert-traces/2026-10-02-batch10-profile/refined ~/Desktop/cl1b-traces.zip
set -euo pipefail
stem="$1"; traces="$2"; out="$3"
store="$(dirname "$0")/../cert-fixtures/profiles"
rec="$store/$stem.json"; proc="$traces/$stem.sweep.processes.json"
[ -f "$rec" ]  || { echo "no record $rec"; exit 2; }
[ -f "$proc" ] || { echo "no processes file $proc"; exit 2; }
tmp="$(mktemp -d)"; mkdir -p "$tmp/cert/fixtures" "$tmp/cert/raw"
cp "$rec" "$tmp/cert/fixtures/"                                         # the store's record: detector, pick, map state carried over by the mode
[ -f "$store/$stem.defaults.json" ] && cp "$store/$stem.defaults.json" "$tmp/cert/fixtures/"
cp "$proc" "$tmp/cert/"
n=0; for f in "$traces/raw/$stem."*; do cp "$f" "$tmp/cert/raw/"; n=$((n+1)); done
[ "$n" -gt 0 ] || { echo "no raw captures for $stem under $traces/raw"; exit 2; }
grep -l "config.json" -r "$tmp" && { echo "REFUSED: a trace names config.json"; exit 3; }
grep -rl "/Users/" "$tmp" && { echo "REFUSED: an unredacted home path"; exit 3; }
( cd "$tmp" && zip -rq "$out" cert )
echo "$out: record + $(ls "$tmp/cert/fixtures" | wc -l | tr -d ' ') fixture file(s), processes.json, $n raw captures; version $(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['version'])" "$rec")"
echo "on the other Mac:  cd ~/Library/ejmap && unzip -n $(basename "$out")      (-n: never overwrites that Mac's own files)"
rm -rf "$tmp"
