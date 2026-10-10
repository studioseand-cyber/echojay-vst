#!/bin/bash
# V2 ONLY (18h: loop pills wrap in rows, the [Go] bar fixed, Push it only when short of target, the after-verb "How's it sounding?"
# bubble with Check, the peak GR estimate for third-party limiters; 20 Sep 2026). Pairs with the INSTALLED 19g Link (F0886EBE / 4C1AEDD0), which is NOT reinstalled: this script verifies the installed Link is that build and stops
# if it is not. Usage: bash INSTALL_AAX_2026-09-20h.command [--verify-only] [dir-with-signed-V2-bundle]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/Application Support/Avid/Audio/Plug-Ins"
WANT_V2="CD828DA0-BDD8-3E1B-BDC4-20973F62C9F2"        # V2  x86_64 (Pro Tools under Rosetta loads THIS line) - 20h
WANT_V2_ARM="115EEBEF-A0C2-37C3-9E4D-D96BF2953493"    # V2  arm64 - 20h
WANT_LK="F0886EBE-1710-3FC5-A34A-DE1B09D2BE36"        # Link x86_64 - the INSTALLED 19g Link (unchanged)
WANT_LK_ARM="4C1AEDD0-6B9C-3453-B2D3-D1490E519564"    # Link arm64
BK="$HOME/Desktop/DO_NOT_SIGN_pre20h_backup_2026-09-20"
VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then VERIFY_ONLY=1; shift; fi
ARG_DIR="${1:-}"
locate () {
  local d
  for d in "${ARG_DIR:-}" "$HOME/Desktop/ejsign_2026-09-20h" "$HERE" "$HOME/Desktop"; do
    [ -n "$d" ] && [ -d "$d/$1" ] && { echo "$d/$1"; return 0; }
  done
  return 1
}
verify () {  # $1 bundle, $2 wanted x86_64, $3 label, $4 wanted arm64 -> rc 2 notfound/broken/noslice, 1 mismatch, 0 ok
  local bundle="$1" bin gx ga
  if [ ! -d "$bundle" ]; then echo "  NOT FOUND  $3 - no bundle at $bundle"; return 2; fi
  bin="$bundle/Contents/MacOS/$(basename "$bundle" .aaxplugin)"
  if [ ! -f "$bin" ]; then echo "  BROKEN     $3 - bundle has no binary at Contents/MacOS/"; return 2; fi
  gx="$(dwarfdump --uuid "$bin" 2>/dev/null | awk '/\(x86_64\)/{print $2; exit}')"
  ga="$(dwarfdump --uuid "$bin" 2>/dev/null | awk '/\(arm64\)/{print $2; exit}')"
  if [ -z "$gx" ] || [ -z "$ga" ]; then echo "  NO SLICE   $3 - missing a slice (x86_64=${gx:-none} arm64=${ga:-none})"; return 2; fi
  if [ "$gx" = "$2" ] && [ "$ga" = "$4" ]; then echo "  OK         $3 x86_64 $gx  arm64 $ga"; return 0; fi
  echo "  MISMATCH   $3 x86_64 $gx (want $2)  arm64 $ga (want $4)"; return 1
}
V2SRC="$(locate 'EchoJay V2.aaxplugin')" || { echo "  NOT FOUND  EchoJay V2 - looked in: ${ARG_DIR:-}(arg) ~/Desktop/ejsign_2026-09-20h $HERE ~/Desktop"; exit 2; }
echo "Found V2:   $V2SRC"
echo "Verifying the V2 source bundle and the INSTALLED Link (19g) it pairs with..."
verify "$V2SRC" "$WANT_V2" "EchoJay V2 (2026-09-20h: loop pills wrap, Push it when short, after-verb bubble, peak GR estimate)" "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: V2 source verify failed (rc=$rc). Nothing installed."; exit $rc; }
verify "$DEST/EchoJay Link.aaxplugin" "$WANT_LK" "INSTALLED EchoJay Link (must be the 19g Link this V2 pairs with)" "$WANT_LK_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: the installed Link is not the 18 build (rc=$rc). Install ship_2026-09-18's Link first. Nothing installed."; exit $rc; }
if [ "$VERIFY_ONLY" -eq 1 ]; then echo "VERIFY-ONLY: the V2 bundle is 20h and the installed Link is 19g. (No install.)"; exit 0; fi
mkdir -p "$BK"
if [ -d "$DEST/EchoJay V2.aaxplugin" ]; then
  echo "Backing up installed EchoJay V2.aaxplugin ..."
  rm -rf "$BK/EchoJay V2.aaxplugin"; sudo ditto "$DEST/EchoJay V2.aaxplugin" "$BK/EchoJay V2.aaxplugin" && echo "  backed up -> $BK/EchoJay V2.aaxplugin" || echo "  (backup failed)"
fi
echo "Installing EchoJay V2.aaxplugin with ditto (never cp)..."
sudo ditto "$V2SRC" "$DEST/EchoJay V2.aaxplugin" || { echo "STOP: ditto failed."; exit 1; }
echo "Verifying the installed V2..."
verify "$DEST/EchoJay V2.aaxplugin" "$WANT_V2" "installed EchoJay V2" "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: installed V2 verify failed (rc=$rc)."; exit $rc; }
sudo xattr -cr "$DEST/EchoJay V2.aaxplugin" 2>/dev/null || xattr -cr "$DEST/EchoJay V2.aaxplugin"
echo "  xattr count after -cr on EchoJay V2.aaxplugin: $(xattr -lr "$DEST/EchoJay V2.aaxplugin" 2>/dev/null | grep -c ':')"
echo "DONE. V2 20h installed beside the 19g Link. In Pro Tools (under Rosetta) the loaded V2 image UUID must read x86_64 $WANT_V2"
echo "and the Link x86_64 $WANT_LK."
echo "If a stale host lingers: sudo killall -9 AAXHostService AAEHostService AUHostingService 2>/dev/null || true"
