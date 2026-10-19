#!/bin/bash
# EJ Map - the RATIO LAW spot check (Kathy, 19 Oct 2026). ONE command, from inside this folder:
#     bash ratio_law.sh [--dry-run] [products_sean_mac.txt | products_kathy_mac.txt]
#     (default list: products_sean_mac.txt - the licence-bound ones; --dry-run: the plan only, loads nothing)
# For each product in the list (certified compressors with an adjustable ratio measured at ONE ratio - B's list, 18 Oct): its ratio control at the
# lowest, 2:1, 4:1 and the highest ratio (the measured ratio skipped), at the tone check's g 2 and g 4 positions, a steady tone at 3
# input levels above threshold - the tone check's own probe call with the ratio write swapped. Licence gate first; a window stops that
# product (closed, never clicked). Results: ~/Library/ejmap/cert/ratio_law/ (one .ratio_law.json per product, raw traces in raw/).
# Nothing is sent anywhere.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/ejmap.app/Contents/MacOS/ejmap"
CERT="${EJ_RATIO_LAW_CERT:-$HOME/Library/ejmap/cert}"   # the cert folder whose profiles are checked (your own: ~/Library/ejmap/cert)
DRY=""; [ "${1:-}" = "--dry-run" ] && { DRY="--dry-run"; shift; }
LIST="$HERE/${1:-products_sean_mac.txt}"; [ -f "$LIST" ] || LIST="${1:-}"
[ -f "$LIST" ] || { echo "RATIO-LAW: no list $LIST (products_sean_mac.txt or products_kathy_mac.txt)"; exit 2; }
[ -x "$BIN" ] || { echo "RATIO-LAW: no ejmap.app in $HERE - unzip the whole pack and run this from inside it"; exit 2; }
[ -d "$CERT/profiles" ] || { echo "RATIO-LAW: no profiles in $CERT - this is for the Mac that ran the certification"; exit 2; }
if xattr -lr "$HERE/ejmap.app" 2>/dev/null | grep -q com.apple.quarantine; then
    echo "RATIO-LAW: the app still carries the download's quarantine flag. Run this once, then run ratio_law.sh again:"
    echo "    xattr -dr com.apple.quarantine \"$HERE\""
    exit 2
fi
ONLY=(); while IFS= read -r name; do name="${name%%#*}"; name="$(echo "$name" | sed 's/[[:space:]]*$//')"; [ -n "$name" ] && ONLY+=(--only "$name"); done < "$LIST"
[ ${#ONLY[@]} -gt 0 ] || { echo "RATIO-LAW: $LIST names no product"; exit 2; }
echo "RATIO-LAW: $(( ${#ONLY[@]} / 2 )) product(s) from $(basename "$LIST"); cert folder $CERT"
echo "RATIO-LAW: the plan"
"$BIN" --ratio-law --cert "$CERT" "${ONLY[@]}" --dry-run || { echo "RATIO-LAW: the plan refused (see above) - nothing was run"; exit 3; }
[ -n "$DRY" ] && { echo "RATIO-LAW: --dry-run: nothing loaded"; exit 0; }
echo "RATIO-LAW: preflight"
"$BIN" --cert-preflight || { echo "RATIO-LAW: the preflight refused (mains power, the probe's signature) - nothing was run"; exit 3; }
mkdir -p "$CERT/ratio_law"
caffeinate -dimsu "$BIN" --ratio-law --cert "$CERT" "${ONLY[@]}" 2>&1 | tee -a "$CERT/ratio_law/ratio_law_19oct.log"
echo "RATIO-LAW: finished (exit ${PIPESTATUS[0]}). Send Kathy the results folder:"
echo "    cd \"$CERT\" && zip -rq ~/Desktop/ratio_law_19oct.zip ratio_law"
