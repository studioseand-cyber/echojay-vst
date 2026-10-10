#!/bin/bash
# Install the SIGNED 24 Sep 2026 "r" build of EchoJay V2 AND EchoJay Link (the Listen button is back on the arm bubble and
# appears once per build; a stepped control is dialled BY NAME and verified against the plugin own display; the group fader is a
# VCA that stays where it is put; a group can be the chat target; the channel question is asked once. On top of 21q. SIGN THE HELPER FIRST: see SIGN_HELPER_2026-09-21r.command.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/Application Support/Avid/Audio/Plug-Ins"

# THE FOUR SLICE UUIDs of this build (preserved through wraptool signing).
WANT_V2="0A4618DD-AECD-3A24-A630-8632F17C76BA"        # V2  x86_64 (Pro Tools under Rosetta loads THIS line)
WANT_V2_ARM="25F20AAB-107F-3AC2-A222-70BBABB58C0D"    # V2  arm64
WANT_LK="AC3D6E5C-B0C1-37C2-9147-4AE136C2B272"        # Link x86_64
WANT_LK_ARM="9F8BB141-6FF6-3C94-BA93-482193840BF1"    # Link arm64

BK="$HOME/Desktop/DO_NOT_SIGN_pre21r_backup_2026-09-24"

VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then VERIFY_ONLY=1; shift; fi
ARG_DIR="${1:-}"

locate () {  # $1 = bundle filename
  local d
  for d in "${ARG_DIR:-}" "$HOME/Desktop/ejsign_2026-09-21r" "$HERE" "$HOME/Desktop"; do
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
V2SRC="$(locate 'EchoJay V2.aaxplugin')"   || { echo "  NOT FOUND  EchoJay V2 - looked in: ${ARG_DIR:-}(arg) ~/Desktop/ejsign_2026-09-21r $HERE ~/Desktop"; exit 2; }
LKSRC="$(locate 'EchoJay Link.aaxplugin')" || { echo "  NOT FOUND  EchoJay Link - looked in: ${ARG_DIR:-}(arg) ~/Desktop/ejsign_2026-09-21r $HERE ~/Desktop"; exit 2; }
echo "Found V2:   $V2SRC"
echo "Found Link: $LKSRC"
echo "Verifying BOTH source bundles before install..."
verify "$V2SRC" "$WANT_V2" "EchoJay V2 (2026-09-21r: the Listen button, named positions, the group VCA, groups as a chat target, the channel answer)"   "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: V2 source verify failed (rc=$rc). Nothing installed."; exit $rc; }
verify "$LKSRC" "$WANT_LK" "EchoJay Link (2026-09-21r: the Listen button, named positions, the group VCA, groups as a chat target, the channel answer)" "$WANT_LK_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: Link source verify failed (rc=$rc). Nothing installed."; exit $rc; }

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
# Clear extended attributes (com.apple.quarantine rides an AirDrop/zip transfer) on BOTH
# installed bundles; print what remains (must be 0).
for name in "EchoJay V2.aaxplugin" "EchoJay Link.aaxplugin"; do
  sudo xattr -cr "$DEST/$name" 2>/dev/null || xattr -cr "$DEST/$name"
  echo "  xattr count after -cr on $name: $(xattr -lr "$DEST/$name" 2>/dev/null | grep -c ':')"
done
echo "DONE. Both installed. In Pro Tools (under Rosetta) the loaded V2 image UUID must read x86_64 $WANT_V2"
echo "and the Link x86_64 $WANT_LK."
echo "If a stale host lingers: sudo killall -9 AAXHostService AAEHostService AUHostingService 2>/dev/null || true"
