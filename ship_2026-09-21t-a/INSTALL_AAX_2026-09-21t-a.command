#!/bin/bash
# Install the SIGNED 25 Sep 2026 "t-a" HOTFIX build of EchoJay V2 (every reply control - Build, Apply, the pills -
# can be pressed again on every tab; the Working-on menu lists the groups beside the Links and choosing a Link
# leaves the group; a group chat gets its own folder; the chain MIX knob writes the rack on screen, borrowed or
# not. On top of 21s. SIGN THE HELPER FIRST: see SIGN_HELPER_2026-09-21t-a.command.
#
# V2 ONLY, and that is the point: 21t-a changed the V2 editor and the V2 processor, no Link source at all, so the
# Link binary IS the one 21s shipped. It is not re-signed and not re-installed - but it IS verified in place, by
# its own two UUIDs, because "V2 moved and Link did not" is only safe if the Link that is installed is the 21s one.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/Application Support/Avid/Audio/Plug-Ins"

# THE TWO SLICE UUIDs of this build (preserved through wraptool signing).
WANT_V2="2C33EA2F-C255-388A-8712-62F2AF121FF0"        # V2  x86_64 (Pro Tools under Rosetta loads THIS line)
WANT_V2_ARM="D9377652-FBAB-3AD9-9E5F-A628CB4DC693"    # V2  arm64
# The Link that must already be installed - 21s's, unchanged this round.
WANT_LK="C376360C-55C6-334B-B0F4-81A812F06FCB"        # Link x86_64
WANT_LK_ARM="79E22C89-C637-3DD9-BD4E-436661EABB75"    # Link arm64

BK="$HOME/Desktop/DO_NOT_SIGN_pre21t-a_backup_2026-09-25"

VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then VERIFY_ONLY=1; shift; fi
ARG_DIR="${1:-}"

locate () {  # $1 = bundle filename
  local d
  for d in "${ARG_DIR:-}" "$HOME/Desktop/ejsign_2026-09-21t-a" "$HOME/Desktop/ejsign_2026-09-21p" "$HERE" "$HOME/Desktop"; do
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

# --- locate + verify the V2 source bundle before touching anything ----------
V2SRC="$(locate 'EchoJay V2.aaxplugin')" || { echo "  NOT FOUND  EchoJay V2 - looked in: ${ARG_DIR:-}(arg) ~/Desktop/ejsign_2026-09-21t-a ~/Desktop/ejsign_2026-09-21p $HERE ~/Desktop"; exit 2; }
echo "Found V2:   $V2SRC"
echo "Verifying the V2 source bundle before install..."
verify "$V2SRC" "$WANT_V2" "EchoJay V2 (2026-09-21t-a: the reply controls can be pressed, a group is a target you can choose and leave, the chain MIX knob writes the rack on screen)" "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: V2 source verify failed (rc=$rc). Nothing installed."; exit $rc; }

# --- the Link stays where it is, and is CHECKED where it is -----------------
echo "Checking the installed Link is still 21s's (unchanged this round)..."
verify "$DEST/EchoJay Link.aaxplugin" "$WANT_LK" "installed EchoJay Link (21s, unchanged in 21t-a)" "$WANT_LK_ARM"; lrc=$?
if [ $lrc -ne 0 ]; then
  echo "STOP: the installed Link is not the 21s build this V2 is paired with (rc=$lrc). Nothing installed."
  echo "      Install 21s's Link first, or ask for a Link build in the next round."
  exit $lrc
fi

if [ "$VERIFY_ONLY" -eq 1 ]; then echo "VERIFY-ONLY: the V2 source bundle is the intended build and the installed Link is 21s's. (No install.)"; exit 0; fi

# --- back up the currently-installed V2, then install V2 --------------------
mkdir -p "$BK"
name="EchoJay V2.aaxplugin"
if [ -d "$DEST/$name" ]; then
  echo "Backing up installed $name ..."
  rm -rf "$BK/$name"; sudo ditto "$DEST/$name" "$BK/$name" && echo "  backed up -> $BK/$name" || echo "  (backup failed for $name)"
fi
echo "Installing $name with ditto (never cp)..."
sudo ditto "$V2SRC" "$DEST/$name" || { echo "STOP: ditto failed for $name."; exit 1; }

echo "Verifying the installed pair (new V2, unchanged Link)..."
verify "$DEST/EchoJay V2.aaxplugin"   "$WANT_V2" "installed EchoJay V2"   "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: installed V2 verify failed (rc=$rc)."; exit $rc; }
verify "$DEST/EchoJay Link.aaxplugin" "$WANT_LK" "installed EchoJay Link" "$WANT_LK_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: installed Link verify failed (rc=$rc)."; exit $rc; }
# Clear extended attributes (com.apple.quarantine rides an AirDrop/zip transfer) on the bundle
# that was just installed; print what remains (must be 0).
sudo xattr -cr "$DEST/$name" 2>/dev/null || xattr -cr "$DEST/$name"
echo "  xattr count after -cr on $name: $(xattr -lr "$DEST/$name" 2>/dev/null | grep -c ':')"
echo "DONE. V2 installed; the Link is unchanged and verified in place. In Pro Tools (under Rosetta) the loaded V2"
echo "image UUID must read x86_64 $WANT_V2, and the Link x86_64 $WANT_LK."
echo "If a stale host lingers: sudo killall -9 AAXHostService AAEHostService AUHostingService 2>/dev/null || true"
