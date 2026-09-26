#!/bin/bash
# state_path_guard (26 Sep 2026 ruling: EVERY user-state path resolves through EJStateRoot).
#
# WHY A GREP AND NOT A RUNTIME CHECK. The runtime half exists too - run_guard.sh runs every guard under a
# sandbox that DENIES writes to the live folders, and fingerprints them before and after. But a sandbox can only
# stop the write it sees, and a fingerprint cannot attribute a change while a live host is running (the plugin
# rewrites its lock, lease and rolling log every few seconds). This guard is the structural half: a state path
# that does not go through echojay::userDocuments() / userAppData() / userStateHome() cannot be isolated by
# ANY runner, because juce::userDocumentsDirectory and userApplicationDataDirectory resolve from the password
# database and ignore $HOME. That is how a harness came to write into the live ~/Documents/EchoJay and evict the
# dev-mode dumps of real sessions.
#
# ALLOWED, each for a stated reason (read-only INPUTS and user-facing destinations are not state):
#   PluginEditor.cpp  the installer download destination (~/Downloads, falling back to ~/Documents) - the USER's
#                     download, outside Documents/EchoJay; a copy inside a test root is a copy nobody can find.
#   PluginEditor.cpp  the "Add Plugin Scan Folder" chooser's start directory - a starting point, not a write.
#   PluginScanner.cpp / EJScanFreshness.h / EJPaceCheck.h  the installed plug-in folders the scanner walks.
#   EJStateRoot.h     the redirect itself.
cd "${EJ_SRC_ROOT:-$(dirname "$0")/../..}"   # EJ_SRC_ROOT: a pre-round checkout for the RED run

RC=0

# --- 1. no raw Documents location outside the allowlist ---------------------------------------------------
DOCS="$(grep -n "getSpecialLocation *( *juce::File::userDocumentsDirectory" Source/*.cpp Source/*.h 2>/dev/null \
        | grep -v "EJStateRoot.h:" \
        | grep -v "downloadsDir = juce::File::getSpecialLocation" )"
if [ -n "$DOCS" ]; then
  echo "RAW ~/Documents, outside EJStateRoot (state must go through echojay::userDocuments()):"
  echo "$DOCS" | sed 's/^/  /'
  RC=1
fi

# --- 2. no raw Application Support location outside EJStateRoot -------------------------------------------
APPD="$(grep -n "getSpecialLocation *( *juce::File::userApplicationDataDirectory" Source/*.cpp Source/*.h 2>/dev/null \
        | grep -v "EJStateRoot.h:" )"
if [ -n "$APPD" ]; then
  echo "RAW ~/Library, outside EJStateRoot (state must go through echojay::userAppData()):"
  echo "$APPD" | sed 's/^/  /'
  RC=1
fi

# --- 3. a home-directory path that WRITES under Library/ must go through userStateHome() ------------------
# The scanner's plug-in folders and a chooser's start directory are inputs; ~/Library/<anything EchoJay> is state.
# The path often continues on the NEXT line (.getChildFile("Library/Logs/EchoJay/...")), so the window is two
# lines - a one-line grep missed exactly that shape on the review-zorder log.
HOMEW="$(grep -n -A1 "getSpecialLocation *( *juce::File::userHomeDirectory" Source/*.cpp Source/*.h 2>/dev/null \
        | grep -v "EJStateRoot.h[-:]" \
        | awk -F'[-:]' '/getSpecialLocation/ {file=$1; line=$2; buf=$0; getline nxt; if ((buf nxt) ~ /[Ee]choJay|\.echojay/) print file":"line": "buf}' \
        | grep -v "Library/Audio/Plug-Ins" )"
if [ -n "$HOMEW" ]; then
  echo "RAW ~ for an EchoJay path (state must go through echojay::userStateHome()):"
  echo "$HOMEW" | sed 's/^/  /'
  RC=1
fi

# --- 4. the redirects themselves must still exist and still honour the override -------------------------
for fn in userDocuments userAppData userStateHome; do
  grep -q "inline juce::File $fn()" Source/EJStateRoot.h || { echo "EJStateRoot.h no longer defines $fn()"; RC=1; }
done
grep -q "stateIsIsolated()" Source/EJStateRoot.h || { echo "EJStateRoot.h no longer reads the override"; RC=1; }
# ...and each one must BRANCH on it - a redirect that ignores ECHOJAY_STATE_HOME is the defect this guard exists for.
python3 - <<'PY' || RC=1
import re, sys
s = open('Source/EJStateRoot.h').read()
bad = []
for fn in ('userDocuments', 'userAppData', 'userStateHome'):
    m = re.search(r'inline juce::File ' + fn + r'\(\)\s*\{(.*?)\n\}', s, re.S)
    if m is None or 'stateIsIsolated()' not in m.group(1):
        bad.append(fn)
if bad:
    print('  these redirects do not branch on the override: ' + ', '.join(bad))
    sys.exit(1)
PY

if [ $RC -ne 0 ]; then
  echo; echo "==== state_path_guard: RED (a user-state path that no runner can isolate) ===="
  exit 1
fi
echo "==== state_path_guard: GREEN (every user-state path resolves through EJStateRoot) ===="
