#!/bin/bash
# notarise_and_staple.sh - after build_beta_for_staging.sh (--sign, Developer ID): notarise and staple the Beta app AND its disk image,
# then prove it on a copy flagged as an email download would flag it (Kathy, 26 Oct; NOTARISE_FOR_SEAN.md).
#
#   notarise_and_staple.sh "<folder holding EchoJay Wizard Beta.app>" [--profile EchoJayNotarize] [--app-name "EchoJay Wizard Beta"]
#   notarise_and_staple.sh --check [--profile EchoJayNotarize]     (only asks Apple whether the profile can notarise; submits nothing)
#
# The credentials live ONLY in the login keychain profile (xcrun notarytool store-credentials, which Sean runs himself). This script
# never takes, prints or stores a password or key. It stops at the first failure and prints Apple's log for a rejected submission.
set -euo pipefail
PROFILE="EchoJayNotarize"; NAME="EchoJay Wizard Beta"; DIR=""; CHECK=0
while [ $# -gt 0 ]; do case "$1" in --profile) PROFILE="$2"; shift;; --app-name) NAME="$2"; shift;; --check) CHECK=1;; *) DIR="$1";; esac; shift; done
die() { echo "STOP: $*" >&2; exit 1; }

hist="$(xcrun notarytool history --keychain-profile "$PROFILE" 2>&1)" || die "the profile '$PROFILE' cannot notarise yet: $(head -1 <<< "$hist")"
echo "PASS  profile '$PROFILE' answers (Apple accepts it)"
[ "$CHECK" = 1 ] && exit 0

[ -n "$DIR" ] || die "give the folder that holds '$NAME.app'"
APP="$DIR/$NAME.app"; DMG="$DIR/$NAME.dmg"
[ -d "$APP" ] || die "no app at $APP"
codesign --verify --strict --deep "$APP" || die "the app does not verify - rebuild with build_beta_for_staging.sh --sign"
codesign -dv "$APP" 2>&1 | grep -q "^TeamIdentifier=8BT5F9B887" || die "the app is not Developer ID signed (team 8BT5F9B887)"
ID="$(codesign -dv "$APP" 2>&1 | sed -n 's/^Authority=\(Developer ID Application:.*\)$/\1/p' | head -1)"

submit() {   # $1 = file to submit; waits; prints the id; on anything but Accepted prints Apple's log and stops
    local out id st
    out="$(xcrun notarytool submit "$1" --keychain-profile "$PROFILE" --wait 2>&1)" || true
    id="$(sed -n 's/^ *id: //p' <<< "$out" | head -1)"; st="$(sed -n 's/^ *status: //p' <<< "$out" | tail -1)"
    echo "      submission $id: $st"
    if [ "$st" != "Accepted" ]; then
        [ -n "$id" ] && xcrun notarytool log "$id" --keychain-profile "$PROFILE" 2>&1 | head -60
        die "Apple did not accept $(basename "$1")"
    fi
}

# 1. the app: zip, submit, staple
Z="$(mktemp -d)/$NAME.zip"; ditto -c -k --keepParent "$APP" "$Z"
echo "1. submitting the app"; submit "$Z"
xcrun stapler staple "$APP" > /dev/null && xcrun stapler validate "$APP" > /dev/null || die "stapling the app failed"
echo "PASS  1. the app is notarised and stapled"

# 2. the disk image, made again from the stapled app, signed, submitted, stapled
STAGE="$(mktemp -d)"; ditto "$APP" "$STAGE/$NAME.app"; ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"; hdiutil create -volname "$NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG" > /dev/null; rm -rf "$STAGE"
codesign -f -s "$ID" --timestamp "$DMG"
echo "2. submitting the disk image"; submit "$DMG"
xcrun stapler staple "$DMG" > /dev/null && xcrun stapler validate "$DMG" > /dev/null || die "stapling the disk image failed"
echo "PASS  2. the disk image is notarised and stapled"

# 3. the proof: a copy flagged as an email / browser download flags it, then Gatekeeper's verdict
C="$(mktemp -d)"; cp "$DMG" "$C/"; xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Mail;" "$C/$NAME.dmg"
spctl -a -vvv -t open --context context:primary-signature "$C/$NAME.dmg" 2>&1 | tee "$C/dmg.txt" | grep -q "source=Notarized Developer ID" || die "Gatekeeper does not accept the flagged disk image: $(cat "$C/dmg.txt")"
MNT="$(mktemp -d)"; hdiutil attach -nobrowse -readonly -mountpoint "$MNT" "$C/$NAME.dmg" > /dev/null
ditto "$MNT/$NAME.app" "$C/$NAME.app"; hdiutil detach "$MNT" > /dev/null
find "$C/$NAME.app" -exec xattr -w com.apple.quarantine "0083;$(printf %x "$(date +%s)");Mail;" {} \;
spctl -a -vvv -t exec "$C/$NAME.app" 2>&1 | tee "$C/app.txt" | grep -q "source=Notarized Developer ID" || die "Gatekeeper does not accept the flagged app: $(cat "$C/app.txt")"
echo "PASS  3. a flagged copy of the disk image and of the app it holds: accepted, source=Notarized Developer ID"
shasum -a 256 "$DMG"
echo "DONE: $DMG is ready to give to testers"
