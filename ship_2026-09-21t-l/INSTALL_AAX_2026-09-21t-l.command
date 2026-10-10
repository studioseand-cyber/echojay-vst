#!/bin/bash
# Install the SIGNED 29 Sep 2026 "21t-l" build of EchoJay V2 AND EchoJay Link. BOTH BUNDLES CHANGED. It goes on
# top of THE INSTALLED 21t-k - read off the AAX folder today, not off a note: V2 49F30609 / AA0C2737, Link
# E53D3A89 / 13D36AE3, which are 21t-k's four UUIDs exactly.
# WHAT IS NEW - the nine things Sean found testing 21t-k, by his test number:
#  (1) A group strip shows the COUNT ("14 Links"), one line, with the names in its tooltip - not fourteen names
#      painted over the strip below.
#  (2) A group fader's undo puts the GROUP's own fader back too, and the log line names the group.
#  (3) key_source tells the truth: a build's key, scale and reference must match what [KEY] printed or it is
#      stamped "chat", which the plugin reads as "(from chat)".
#  (4) correction_mode never reads "custom" after a server build - the rung is derived from the live values -
#      and a refused choice param names its choices.
#  (5) A calibration block on an edit turn waits for the plugin to load instead of being judged six seconds
#      before it exists.
#  (6) The [KEY] block sends what the Meters panel is showing - one reading, not two instants.
#  (7) The group repair waits for the first live Link and never runs against an empty registry.
#  (8) A level match writes a BORROWED member's trim directly, so it is no longer the one member that misses.
#  (9) The headroom cap is the minimum true-peak room across EVERY selected member, and the note names the
#      member that capped it. The headroom op now applies to trims at all.
# SIGN THE HELPERS FIRST (both of them): see SIGN_HELPER_2026-09-21t-l.command.
# BY-HAND CHECKS after installing: see INSTALL_NOTES_2026-09-21t-l.md beside this script.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="/Library/Application Support/Avid/Audio/Plug-Ins"

# THE FOUR SLICE UUIDs of tonight's pair (preserved through wraptool signing).
WANT_V2="27241420-FC10-3F01-99DB-69CCD47D6A85"        # V2   x86_64 (Pro Tools under Rosetta loads THIS line)
WANT_V2_ARM="24B4881D-FBC9-30DD-9A0D-D40C0486F4CB"    # V2   arm64
WANT_LK="0ECB31F9-467E-3780-AF96-9F7C68D875AA"        # Link x86_64
WANT_LK_ARM="131FFC28-2144-3602-B943-FF420AE22629"    # Link arm64

BK="$HOME/Desktop/DO_NOT_SIGN_pre21t-l_backup_2026-09-29"

VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then VERIFY_ONLY=1; shift; fi
ARG_DIR="${1:-}"

locate () {  # $1 = bundle filename
  local d
  # THE SEARCH PATH IS TWO PLACES (21t-g, 26 Sep 2026 ruling): the directory passed explicitly, and the signed
  # folder. $HERE and ~/Desktop are GONE. $HERE is the ship folder, which holds the UNSIGNED staging bundles with
  # exactly the right UUIDs - so a run of this script from inside the package verified OK on every UUID and had
  # only the codesign gate between it and installing an unsigned plugin. A gate is not a search path.
  for d in "${ARG_DIR:-}" "$HOME/Desktop/ejsign_2026-09-21t-l"; do
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

# --- locate + verify BOTH staged bundles before touching anything -----------
V2SRC="$(locate 'EchoJay V2.aaxplugin')"   || { echo "  NOT FOUND  EchoJay V2 - looked in: ${ARG_DIR:-}(arg) and ~/Desktop/ejsign_2026-09-21t-l ONLY (the ship folder and ~/Desktop are deliberately not searched: they hold unsigned copies with the right UUIDs)"; exit 2; }
LKSRC="$(locate 'EchoJay Link.aaxplugin')" || { echo "  NOT FOUND  EchoJay Link - looked in: ${ARG_DIR:-}(arg) and ~/Desktop/ejsign_2026-09-21t-l ONLY (the ship folder and ~/Desktop are deliberately not searched)"; exit 2; }
echo "Found V2:   $V2SRC"
echo "Found Link: $LKSRC"
echo "Verifying BOTH staged bundles before install..."
verify "$V2SRC" "$WANT_V2" "EchoJay V2 (2026-09-21t-l)"   "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: V2 verify failed (rc=$rc). Nothing installed."; exit $rc; }
verify "$LKSRC" "$WANT_LK" "EchoJay Link (2026-09-21t-l)" "$WANT_LK_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: Link verify failed (rc=$rc). Nothing installed."; exit $rc; }

if [ "$VERIFY_ONLY" -eq 1 ]; then echo "VERIFY-ONLY: both staged bundles are the intended build. (No install.)"; exit 0; fi

# --- SIGNED, NOT JUST THE RIGHT BUILD (25 Sep 2026) -------------------------
# The UUIDs prove WHICH build this is. They do not prove it was signed, and an unsigned bundle carrying exactly
# these UUIDs exists on this Mac (the staging folder the zips were made from). Installing one would put a plugin
# Pro Tools refuses to load - or worse, one PACE refuses - into the AAX folder while every check above said OK.
echo "Checking BOTH source bundles are SIGNED..."
for pair in "EchoJay V2|$V2SRC" "EchoJay Link|$LKSRC"; do
  lbl="${pair%%|*}"; bdl="${pair##*|}"
  if ! codesign --verify --deep --strict "$bdl" >/dev/null 2>&1; then
    echo "  UNSIGNED   $lbl - the UUIDs match but codesign --verify --deep --strict FAILS on $bdl"
    echo "STOP: that is the unsigned staging copy, not the signed build. Nothing installed."
    exit 5
  fi
  auth="$(codesign -dv "$bdl" 2>&1 | awk -F= '/^Authority=/{print $2; exit}')"
  echo "  SIGNED     $lbl - valid on disk, satisfies its Designated Requirement (${auth:-authority unavailable})"
done

# --- PRO TOOLS MUST BE QUIT (25 Sep 2026) -----------------------------------
# The 21t-b install went in with Pro Tools running: the files changed on disk and the running instance kept the
# previous build, so "installed" and "what you are testing" disagreed for the rest of the session. Refused here.
if pgrep -f "Pro Tools" >/dev/null 2>&1; then
  echo "STOP: Pro Tools is running. Quit it and run this again - installing underneath it leaves the running"
  echo "      instance on the OLD build while the folder says otherwise."
  exit 4
fi
for svc in AAXHostService AAEHostService AUHostingService; do
  if pgrep -f "$svc" >/dev/null 2>&1; then
    echo "STOP: $svc is still running (a host service outlives Pro Tools by a few seconds). Wait, then re-run."
    exit 4
  fi
done


# --- back up the installed pair, then install LINK FIRST, then V2 -----------
# Order matters: the V2 talks to the Link, so the Link is in place before the V2 that expects it.
mkdir -p "$BK"
for name in "EchoJay Link.aaxplugin" "EchoJay V2.aaxplugin"; do
  if [ -d "$DEST/$name" ]; then
    echo "Backing up installed $name ..."
    rm -rf "$BK/$name"; sudo ditto "$DEST/$name" "$BK/$name" && echo "  backed up -> $BK/$name" || echo "  (backup failed for $name)"
  fi
done
echo "Installing EchoJay Link (21t-l) with ditto (never cp)..."
sudo ditto "$LKSRC" "$DEST/EchoJay Link.aaxplugin" || { echo "STOP: ditto failed for the Link."; exit 1; }
echo "Installing EchoJay V2 (21t-l) with ditto..."
sudo ditto "$V2SRC" "$DEST/EchoJay V2.aaxplugin"   || { echo "STOP: ditto failed for V2."; exit 1; }

echo "Verifying BOTH installed bundles..."
verify "$DEST/EchoJay Link.aaxplugin" "$WANT_LK" "installed EchoJay Link" "$WANT_LK_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: installed Link verify failed (rc=$rc)."; exit $rc; }
verify "$DEST/EchoJay V2.aaxplugin"   "$WANT_V2" "installed EchoJay V2"   "$WANT_V2_ARM"; rc=$?; [ $rc -eq 0 ] || { echo "STOP: installed V2 verify failed (rc=$rc)."; exit $rc; }
# Clear extended attributes (com.apple.quarantine rides an AirDrop/zip transfer) on both; print what remains (0).
for name in "EchoJay Link.aaxplugin" "EchoJay V2.aaxplugin"; do
  sudo xattr -cr "$DEST/$name" 2>/dev/null || xattr -cr "$DEST/$name"
  echo "  xattr count after -cr on $name: $(xattr -lr "$DEST/$name" 2>/dev/null | grep -c ':')"
done
echo "DONE. Both installed. In Pro Tools (under Rosetta) the loaded V2 image UUID must read x86_64 $WANT_V2"
echo "and the Link x86_64 $WANT_LK."
echo "If a stale host lingers: sudo killall -9 AAXHostService AAEHostService AUHostingService 2>/dev/null || true"
