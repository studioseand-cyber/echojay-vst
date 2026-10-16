#!/bin/bash
# EJ Map - the held units' re-check (Kathy, 16 Oct 2026). ONE command:
#     bash recheck.sh
# It uses the ejmap.app in this folder, checks it is ready (the preflight), then re-checks ONLY the units in held_units.txt
# in Sean's own cert folder (~/Library/ejmap/cert): each one is re-derived, re-exported and tone-checked by this build,
# then given any accuracy pass it is missing. It stops by itself at 07:00; run the same command again to carry on.
# Nothing is sent anywhere.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/ejmap.app/Contents/MacOS/ejmap"
LIST="$HERE/held_units.txt"
CERT="$HOME/Library/ejmap/cert"
[ -x "$BIN" ] || { echo "RECHECK: no ejmap.app in $HERE - unzip the whole pack and run this from inside it"; exit 2; }
[ -d "$CERT" ] || { echo "RECHECK: no cert folder at $CERT - this is for the Mac that ran the certification"; exit 2; }
if xattr -lr "$HERE/ejmap.app" 2>/dev/null | grep -q com.apple.quarantine; then
    echo "RECHECK: the app still carries the download's quarantine flag. Run this once, then run recheck.sh again:"
    echo "    xattr -dr com.apple.quarantine \"$HERE\""
    exit 2
fi
echo "RECHECK: preflight"
"$BIN" --cert-preflight || { echo "RECHECK: the preflight refused (see above: mains power, the probe's signature) - nothing was run. Fix it and run recheck.sh again."; exit 3; }
ONLY=()
while IFS= read -r name; do [ -n "$name" ] && ONLY+=(--only "$name"); done < "$LIST"
echo "RECHECK: $(( ${#ONLY[@]} / 2 )) units from held_units.txt; log in $CERT/run_all/"
mkdir -p "$CERT"
caffeinate -dimsu "$BIN" --run-all --jobs 2 \
    --steps preflight,followup,accuracy_gaps_drafts,accuracy_gaps,drafts \
    "${ONLY[@]}" --until 07:00 --out "$CERT" 2>&1 | tee -a "$CERT/recheck_16oct.log"
echo "RECHECK: finished or stopped (exit ${PIPESTATUS[0]}). Morning: see RUNBOOK step 4."
