#!/bin/bash
# EchoJay ship_2026-09-18b - V2 ONLY (crash fix: ChainJsonView, 18 Sep 2026). Pairs with the INSTALLED 18 Link
# (BBAA5ADE / B63ACF61), which is NOT reinstalled: this script verifies the installed Link is that build and stops
# if it is not. Usage: bash INSTALL_AAX_2026-09-18b.command [--verify-only] [dir-with-signed-V2-bundle]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/Application Support/Avid/Audio/Plug-Ins"
WANT_V2="5FBE2A9F-FF98-305F-B9DC-A0452F593331"        # V2  x86_64 (Pro Tools under Rosetta loads THIS line) - 18b
WANT_V2_ARM="B66A6381-B5FC-3F72-8721-434270AFD838"    # V2  arm64 - 18b
WANT_LK="BBAA5ADE-1AF3-3E9F-99C2-60798180243D"        # Link x86_64 - the INSTALLED 18 Link (unchanged)
WANT_LK_ARM="B63ACF61-B4DE-3D5B-840E-BAED5C418D65"    # Link arm64
BK="$HOME/Desktop/DO_NOT_SIGN_pre18b_backup_2026-09-18"
VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then VERIFY_ONLY=1; shift; fi
ARG_DIR="${1:-}"
locate () {
  local d
  for d in "${ARG_DIR:-}" "$HOME/Desktop/ejsign_2026-09-18b" "$HERE" "$HOME/Desktop"; do
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
V2SRC="$(locate 'EchoJay V2.aaxplugin')" || { echo "  NOT FOUND  EchoJay V2 - looked in: ${ARG_DIR:-}(arg) ~/Desktop/ejsign_2026-09-18b $HERE ~/Desktop"; exit 2; }
echo "Found V2:   $V2SRC"
echo "Verifying the V2 source bundle and the INSTALLED Link (18) it pairs with..."
verify "$V2SRC" "$WANT_V2" "EchoJay V2 (2026-09-18b: ChainJsonView crash fix, 6.7 s classify budget)" "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: V2 source verify failed (rc=$rc). Nothing installed."; exit $rc; }
verify "$DEST/EchoJay Link.aaxplugin" "$WANT_LK" "INSTALLED EchoJay Link (must be the 18 Link this V2 pairs with)" "$WANT_LK_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: the installed Link is not the 18 build (rc=$rc). Install ship_2026-09-18's Link first. Nothing installed."; exit $rc; }
if [ "$VERIFY_ONLY" -eq 1 ]; then echo "VERIFY-ONLY: the V2 bundle is 18b and the installed Link is 18. (No install.)"; exit 0; fi
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
echo "DONE. V2 18b installed beside the 18 Link. In Pro Tools (under Rosetta) the loaded V2 image UUID must read x86_64 $WANT_V2"
echo "and the Link x86_64 $WANT_LK."
echo "If a stale host lingers: sudo killall -9 AAXHostService AAEHostService AUHostingService 2>/dev/null || true"
