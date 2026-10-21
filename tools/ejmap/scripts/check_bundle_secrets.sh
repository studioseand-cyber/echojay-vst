#!/bin/bash
# check_bundle_secrets.sh <App.app | folder> - NO SECRETS IN THE APP (Kathy, 21 Oct, contract v1): every file's printable strings are
# searched for anything shaped like a credential - a private key, a cloud / payment / chat key, a JWT, a Bearer token, or a token / secret
# / api key field WITH a value. A search that itself fails exits 3, never "clean". The OAuth client id is public and allowed; the field NAMES the code uses ("access_token", "client_id") are
# allowed when they carry no value. Exit 0 = clean; 1 = something found (printed, cut short); 2 = usage; 3 = the search failed.
set -uo pipefail
T="${1:-}"; [ -e "$T" ] || { echo "usage: check_bundle_secrets.sh <App.app | folder>"; exit 2; }
PATTERNS='-----BEGIN [A-Z ]*PRIVATE KEY-----|sk_(live|test)_[0-9A-Za-z]{10,}|AKIA[0-9A-Z]{16}|gh[pousr]_[0-9A-Za-z]{30,}|xox[baprs]-[0-9A-Za-z-]{10,}|eyJ[0-9A-Za-z_-]{10,}\.eyJ[0-9A-Za-z_-]{10,}|Bearer [0-9A-Za-z._~+/=-]{20,}|(client_secret|api[_-]?key|secret[_-]?key|access_token|refresh_token)["'"'"']? *[:=] *["'"'"']?[0-9A-Za-z._~+/=-]{12,}'
found=0; n=0
while IFS= read -r -d '' f; do
    n=$((n+1))
    hits=$(strings -a -n 8 "$f" 2>/dev/null | grep -Eo -e "$PATTERNS"; rc=${PIPESTATUS[1]}; [ "$rc" -le 1 ] || echo "GREP_FAILED $rc")
    if [[ "$hits" == *GREP_FAILED* ]]; then echo "check_bundle_secrets: the search itself failed on ${f#$T/} - NOT a clean result"; exit 3; fi   # a failing search is never "clean"
    hits=$(head -5 <<< "$hits")
    if [ -n "$hits" ]; then found=1; while IFS= read -r h; do echo "SECRET? ${f#$T/}: ${h:0:24}..."; done <<< "$hits"; fi
done < <(find "$T" -type f -print0)
echo "check_bundle_secrets: $n file(s) searched in $(basename "$T"): $([ $found = 0 ] && echo clean || echo 'SOMETHING FOUND')"
exit $found
