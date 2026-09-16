#!/bin/bash
# Install the SIGNED 16 Sep 2026 CRASH-FIX build of BOTH EchoJay AAX plugins
# (V2 + Link). This build carries the rack-switch AU crash fix (seed-while-
# detached + release-on-park), which ships in BOTH bundles, so BOTH travel and
# BOTH are verified here. Run AFTER signing on the admin Mac.
#   Normal:      double-click, or: bash "INSTALL_AAX_2026-09-16_crashfix.command" [staging-dir]
#   Verify only: bash "INSTALL_AAX_2026-09-16_crashfix.command" --verify-only [staging-dir]  (no install)
# Put the signed "EchoJay V2.aaxplugin" AND "EchoJay Link.aaxplugin" in any of:
# a dir you pass, ~/Desktop/ejsign_2026-09-16_crashfix, this folder, ~/Desktop.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/Application Support/Avid/Audio/Plug-Ins"

# THE FOUR SLICE UUIDs of this build (preserved through wraptool signing).
WANT_V2="90C4A6E9-2886-31E4-80EC-B55E1FD8BBF1"        # V2  x86_64 (Pro Tools under Rosetta loads THIS line)
WANT_V2_ARM="EF1E1DBB-5E14-3E1C-B577-E692A5C1B1D6"    # V2  arm64
WANT_LK="261BCE88-07FD-3B8E-B389-60749D94E602"        # Link x86_64
WANT_LK_ARM="B928A8D1-F073-3199-83EC-CC0B8B4CD925"    # Link arm64

BK="$HOME/Desktop/DO_NOT_SIGN_preCrashFix_backup_2026-09-16"

VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then VERIFY_ONLY=1; shift; fi
ARG_DIR="${1:-}"

locate () {  # $1 = bundle filename
  local d
  for d in "${ARG_DIR:-}" "$HOME/Desktop/ejsign_2026-09-16_crashfix" "$HERE" "$HOME/Desktop"; do
    [ -n "$d" ] && [ -d "$d/$1" ] && { echo "$d/$1"; return 0; }
  done
  return 1
}

# FIVE distinct outcomes, never conflated: NOT FOUND / BROKEN / NO SLICE / MISMATCH / OK.
# BOTH slices checked (x86_64 AND arm64): a signer that alters either UUID is caught.
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

# --- locate + verify BOTH source bundles before touching anything -----------
V2SRC="$(locate 'EchoJay V2.aaxplugin')"   || { echo "  NOT FOUND  EchoJay V2 - looked in: ${ARG_DIR:-}(arg) ~/Desktop/ejsign_2026-09-16_crashfix $HERE ~/Desktop"; exit 2; }
LKSRC="$(locate 'EchoJay Link.aaxplugin')" || { echo "  NOT FOUND  EchoJay Link - looked in: ${ARG_DIR:-}(arg) ~/Desktop/ejsign_2026-09-16_crashfix $HERE ~/Desktop"; exit 2; }
echo "Found V2:   $V2SRC"
echo "Found Link: $LKSRC"
echo "Verifying BOTH source bundles before install..."
verify "$V2SRC" "$WANT_V2" "EchoJay V2 (crash-fix)"   "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: V2 source verify failed (rc=$rc). Nothing installed."; exit $rc; }
verify "$LKSRC" "$WANT_LK" "EchoJay Link (crash-fix)" "$WANT_LK_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: Link source verify failed (rc=$rc). Nothing installed."; exit $rc; }

if [ "$VERIFY_ONLY" -eq 1 ]; then echo "VERIFY-ONLY: both source bundles are the intended build. (No install.)"; exit 0; fi

# --- back up BOTH currently-installed, then install BOTH --------------------
mkdir -p "$BK"
for pair in "EchoJay V2.aaxplugin|$V2SRC" "EchoJay Link.aaxplugin|$LKSRC"; do
  name="${pair%%|*}"; src="${pair##*|}"
  if [ -d "$DEST/$name" ]; then
    echo "Backing up installed $name ..."
    rm -rf "$BK/$name"; sudo ditto "$DEST/$name" "$BK/$name" && echo "  backed up -> $BK/$name" || echo "  (backup failed for $name)"
  fi
  echo "Installing $name with ditto (never cp)..."
  sudo ditto "$src" "$DEST/$name" || { echo "STOP: ditto failed for $name."; exit 1; }
done

echo "Verifying BOTH installed bundles..."
verify "$DEST/EchoJay V2.aaxplugin"   "$WANT_V2" "installed EchoJay V2"   "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: installed V2 verify failed (rc=$rc)."; exit $rc; }
verify "$DEST/EchoJay Link.aaxplugin" "$WANT_LK" "installed EchoJay Link" "$WANT_LK_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: installed Link verify failed (rc=$rc)."; exit $rc; }
echo "DONE. Both installed. In Pro Tools (under Rosetta) the loaded V2 image UUID must read x86_64 $WANT_V2"
echo "and the Link x86_64 $WANT_LK."
echo "If a stale host lingers: sudo killall -9 AAXHostService AAEHostService AUHostingService 2>/dev/null || true"
