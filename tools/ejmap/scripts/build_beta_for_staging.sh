#!/bin/bash
# build_beta_for_staging.sh - ONE COMMAND from a staging URL + client id to a signed, verified Beta (Kathy, 26 Oct: staging readiness).
#
#   build_beta_for_staging.sh --url https://<staging host> --client-id <public OAuth client id> --out <folder>
#                             [--sign "Developer ID Application: <Name> (<TEAM>)"] [--dmg] [--ref <git ref>] [--work <folder>] [--keep]
#   build_beta_for_staging.sh --self-test        (the URL / client id / required-fix checks against their cases; builds nothing)
#
# What it does, in order (each step says PASS or stops with the reason):
#   1. checks the URL (https, a host, nothing after it; http only for this Mac's mock: 127.0.0.1 / localhost) and the client id (a
#      public id: letters, digits, . _ - only, 3-128 characters - never anything shaped like a secret);
#   2. checks the ref (default origin/feat/ejmap-wizard) carries every fix the Beta must ship with (REQUIRED below);
#   3. checks the ref out CLEAN into its own worktree (local edits never reach a tester), configures with the URL and client id baked in;
#   4. builds every target the apps need: ejmap, EchoJayProbe, ejmap-wizard-beta, ejmap-wizard-test, ejmap-wizard (the plain app,
#      which the commit gate never builds), and ejmap-roundtrip-test - nice'd, -j4 (a 16 GB Mac);
#   5. runs the suite: 0 failures or it stops;
#   6. signs the probe (Developer ID, hardened runtime, its entitlements), assembles the three apps (wizard_app/assemble.sh: the secrets
#      search, then Developer ID signing when --sign), and a disk image each with --dmg;
#   7. verifies: codesign --verify --strict --deep on each app, the probe's and engine's team, the Beta's binary carries THIS url and
#      client id (and the Test / plain apps carry none), Gatekeeper's assessment recorded (until notarised it says so);
#   8. writes BUILD_INFO.txt beside the apps: the commit, the url, the client id, the suite line, every file's SHA-256.
# Nothing is sent, uploaded or notarised here (NOTARISE_FOR_SEAN.md is the next step). The map store and the cert store are not touched.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
JUCE_DEFAULT="$(cd "$REPO/.." && pwd)/JUCE"

# THE FIXES THE BETA MUST SHIP WITH (commit | what): the build refuses a ref that lacks one
REQUIRED=(
  "c0115ced|a measurement identity in every measured file (24 Oct)"
  "2a6a1868|the short sound check, the twin's licence window, the reads never drop a plugin (25 Oct)"
  "9907768f|the saturation engage search, one-knob amount, abbreviated thresholds, graphic EQ sliders (25 Oct)"
  "f5b9a85e|the threshold pair rules get their readings (SPL IRON), the onset rule, reverb-or-delay, EQ part by part, a limiter with no ceiling (25 Oct)"
  "21eccad4|only a profile carries a measuring span (the staging rehearsal, 26 Oct)"
)

die() { echo "STOP: $*" >&2; exit 1; }
pass() { echo "PASS  $*"; }

url_ok() {   # 0 = usable; prints the reason when not. $2 = "local" allows this Mac's mock (http://127.0.0.1 / localhost)
    local u="$1" mode="${2:-}"
    [[ -n "$u" ]] || { echo "the url is empty"; return 1; }
    [[ "$u" =~ [[:space:]] ]] && { echo "the url has a space"; return 1; }
    if [[ "$u" =~ ^http://(127\.0\.0\.1|localhost)(:[0-9]{1,5})?/?$ ]]; then
        [[ "$mode" == local ]] && return 0
        echo "http://127.0.0.1 / localhost is the mock: only a dry run (--dry-run-local) may use it"; return 1
    fi
    [[ "$u" =~ ^https:// ]] || { echo "the url must be https"; return 1; }
    [[ "$u" =~ ^https://[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?(:[0-9]{1,5})?/?$ ]] || { echo "the url must be https://<host>[:port] with nothing after the host (the app adds /api/wizard/v1/)"; return 1; }
    [[ "$u" =~ ^https://[^/]*\. ]] || { echo "the url's host has no dot"; return 1; }
    return 0
}
client_ok() {
    local c="$1"
    [[ -n "$c" ]] || { echo "the client id is empty"; return 1; }
    [[ "$c" =~ ^[A-Za-z0-9._-]{3,128}$ ]] || { echo "the client id must be 3-128 letters, digits, . _ - (a public id)"; return 1; }
    [[ "$c" =~ ^(sk_|pk_live|ghp_|xox|AKIA|eyJ) ]] && { echo "the client id looks like a secret - the Beta takes a PUBLIC client id only"; return 1; }
    return 0
}
has_fixes() {   # $1 = commit; prints each missing fix; 0 = all present
    local ref="$1" miss=0 e c w
    for e in "${REQUIRED[@]}"; do c="${e%%|*}"; w="${e#*|}"
        if ! git -C "$REPO" merge-base --is-ancestor "$c" "$ref" 2>/dev/null; then echo "missing $c: $w"; miss=1; fi
    done
    return $miss
}

if [[ "${1:-}" == "--self-test" ]]; then
    f=0; t() { local want="$1"; shift; if "$@" >/dev/null; then got=ok; else got=refused; fi; [[ "$got" == "$want" ]] && echo "ok    $want: $*" || { echo "FAIL  wanted $want, got $got: $*"; f=1; }; }
    t ok      url_ok "https://staging.echojay.ai"
    t ok      url_ok "https://staging.echojay.ai/"
    t ok      url_ok "https://staging.echojay.ai:8443"
    t refused url_ok "http://staging.echojay.ai"
    t refused url_ok "https://staging.echojay.ai/api/wizard/v1"
    t refused url_ok "https://staging.echojay.ai?x=1"
    t refused url_ok "https://localhost"
    t refused url_ok "http://127.0.0.1:8791"
    t ok      url_ok "http://127.0.0.1:8791" local
    t ok      url_ok "http://localhost:8791/" local
    t refused url_ok "http://10.0.0.5:8791" local
    t refused url_ok ""
    t refused url_ok "https://staging .echojay.ai"
    t ok      client_ok "echojay-wizard-beta"
    t ok      client_ok "wiz_beta.2026"
    t refused client_ok ""
    t refused client_ok "ab"
    t refused client_ok "has space"
    t refused client_ok "sk_live_abcdefghijkl"
    t refused client_ok "eyJhbGciOiJIUzI1NiJ9"
    t ok      has_fixes "21eccad4"
    t refused has_fixes "f5b9a85e"
    t refused has_fixes "9907768f"
    echo "self-test: $([ $f = 0 ] && echo 'all cases as expected' || echo FAILED)"; exit $f
fi

URL=""; CID=""; OUT=""; SIGN=""; DMG=0; REF="origin/feat/ejmap-wizard"; WORK=""; KEEP=0; LOCAL=""
while [ $# -gt 0 ]; do case "$1" in
    --url) URL="$2"; shift;; --client-id) CID="$2"; shift;; --out) OUT="$2"; shift;; --sign) SIGN="$2"; shift;; --dmg) DMG=1;;
    --ref) REF="$2"; shift;; --work) WORK="$2"; shift;; --keep) KEEP=1;; --dry-run-local) LOCAL=local;;
    *) die "unknown option $1";; esac; shift; done
[ -n "$OUT" ] || die "--out <folder> is required"
why="$(url_ok "$URL" "$LOCAL")" || die "the url: $why"
why="$(client_ok "$CID")" || die "the client id: $why"
pass "1. url $URL, client id $CID"
SHA="$(git -C "$REPO" rev-parse --verify "$REF^{commit}")" || die "no such ref $REF"
missing="$(has_fixes "$SHA")" || die "$REF ($SHA) lacks a fix the Beta must ship with:
$missing"
pass "2. $REF = $SHA carries all ${#REQUIRED[@]} required fixes"

WORK="${WORK:-$(mktemp -d /tmp/ejbeta.XXXXXX)}"; mkdir -p "$WORK"; SRC="$WORK/src"; BLD="$WORK/build"
[ -e "$SRC" ] && die "$SRC exists - give an empty --work folder"
git -C "$REPO" worktree add --detach -q "$SRC" "$SHA"
cleanup() { [ "$KEEP" = 1 ] || git -C "$REPO" worktree remove --force "$SRC" 2>/dev/null || true; }
trap cleanup EXIT
[ -z "$(git -C "$SRC" status --porcelain)" ] || die "the checkout is not clean"
cmake -S "$SRC" -B "$BLD" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DJUCE_PATH="${JUCE_PATH:-$JUCE_DEFAULT}" \
      -DEJ_WIZARD_BETA_SERVER_URL="$URL" -DEJ_WIZARD_BETA_CLIENT_ID="$CID" > "$WORK/configure.log" 2>&1 || die "configure failed ($WORK/configure.log)"
pass "3. clean checkout of $SHA configured (url and client id baked in) -> $BLD"

for t in ejmap EchoJayProbe ejmap-wizard-beta ejmap-wizard-test ejmap-wizard ejmap-roundtrip-test; do
    nice -n 10 cmake --build "$BLD" --target "$t" -j4 > "$WORK/build-$t.log" 2>&1 || die "building $t failed ($WORK/build-$t.log)"
done
pass "4. built ejmap, EchoJayProbe, ejmap-wizard-beta, ejmap-wizard-test, ejmap-wizard (plain), ejmap-roundtrip-test"

SUITE="$( (cd "$SRC" && nice -n 10 "$BLD/tools/ejmap/ejmap-roundtrip-test_artefacts/RelWithDebInfo/ejmap-roundtrip-test" 2>&1) | tee "$WORK/suite.log" | grep -E '^[0-9]+ checks, [0-9]+ failures' | tail -1)"
[[ "$SUITE" =~ ^[0-9]+\ checks,\ 0\ failures$ ]] || die "the suite: '${SUITE:-no summary line}' ($WORK/suite.log)"
pass "5. suite: $SUITE"

ENT="$SRC/tools/au_instantiate_probe/EchoJayProbe.entitlements"; PROBE="$BLD/EchoJayProbe_artefacts/RelWithDebInfo/EchoJayProbe"
ID="${SIGN:-Developer ID Application: Sean Donoghue (8BT5F9B887)}"
codesign -f -s "$ID" --timestamp --options runtime --entitlements "$ENT" "$PROBE" > /dev/null 2>&1 || die "signing the probe with '$ID' failed"
mkdir -p "$OUT"; AS="$SRC/tools/ejmap/wizard_app/assemble.sh"; D=(); [ "$DMG" = 1 ] && D=(--dmg)
S=(); [ -n "$SIGN" ] && S=(--sign "$SIGN")
"$AS" "$BLD" "$OUT" --beta ${S[@]+"${S[@]}"} ${D[@]+"${D[@]}"} > "$WORK/assemble-beta.log" 2>&1 || die "assembling the Beta failed ($WORK/assemble-beta.log)"
"$AS" "$BLD" "$OUT" --test ${S[@]+"${S[@]}"} ${D[@]+"${D[@]}"} > "$WORK/assemble-test.log" 2>&1 || die "assembling the Test app failed ($WORK/assemble-test.log)"
"$AS" "$BLD" "$OUT" ${S[@]+"${S[@]}"} ${D[@]+"${D[@]}"} > "$WORK/assemble-plain.log" 2>&1 || die "assembling the plain app failed ($WORK/assemble-plain.log)"
pass "6. probe signed; Beta, Test and plain apps assembled$([ -n "$SIGN" ] && echo ', Developer ID signed' || echo ' (engine and app ad hoc: give --sign for a tester build)')$([ "$DMG" = 1 ] && echo ', disk images made')"

BETA="$OUT/EchoJay Wizard Beta.app"; TEST="$OUT/EchoJay Wizard Test.app"; PLAIN="$OUT/EchoJay Wizard.app"
for a in "$BETA" "$TEST" "$PLAIN"; do codesign --verify --strict --deep "$a" 2>/dev/null || die "codesign --verify failed on $(basename "$a")"; done
for a in "$BETA" "$TEST" "$PLAIN"; do
    team="$(codesign -dv "$a/Contents/MacOS/EchoJayProbe" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
    [ "$team" = "8BT5F9B887" ] || die "$(basename "$a")'s probe is not Developer ID signed (team '$team')"
done
# the strings go to a file first: under pipefail, grep -q stopping early kills strings (SIGPIPE) and the pipeline reads as "not found"
strs() { local b; b="$(find "$1/Contents/MacOS" -maxdepth 1 -type f -perm -u+x ! -name ejmap ! -name EchoJayProbe)"; strings -a "$b" > "$WORK/strings.txt"; }
strs "$BETA"
grep -qxF -e "$URL" "$WORK/strings.txt" || die "the Beta's binary does not carry the url $URL"
grep -qxF -e "$CID" "$WORK/strings.txt" || die "the Beta's binary does not carry the client id $CID"
for a in "$TEST" "$PLAIN"; do strs "$a"; grep -qF -e "$URL" "$WORK/strings.txt" && die "$(basename "$a") carries the staging url (only the Beta may)"; done
GK="$(spctl --assess --type execute -vv "$BETA" 2>&1 | tr '\n' ' ' || true)"
pass "7. verified: three apps sign-verify strict/deep, probes team 8BT5F9B887, the Beta carries this url + client id, Test / plain carry none"
echo "      Gatekeeper on the Beta: $GK"

{
  echo "EchoJay Wizard Beta - build info ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
  echo "commit:     $SHA ($REF)"
  echo "server url: $URL"
  echo "client id:  $CID (public)"
  echo "suite:      $SUITE"
  echo "signed:     $([ -n "$SIGN" ] && echo "$SIGN" || echo 'probe Developer ID; engine and apps ad hoc')"
  echo "gatekeeper: $GK"
  echo "notarised:  no (NOTARISE_FOR_SEAN.md)"
  echo "files (SHA-256):"
  find "$OUT" -maxdepth 1 -name '*.dmg' -exec shasum -a 256 {} \; | sed "s|$OUT/|  |"
  for a in "$BETA" "$TEST" "$PLAIN"; do find "$a/Contents/MacOS" -type f -exec shasum -a 256 {} \; | sed "s|$OUT/|  |"; done
} > "$OUT/BUILD_INFO.txt"
pass "8. $OUT/BUILD_INFO.txt"
echo "DONE: $OUT"
