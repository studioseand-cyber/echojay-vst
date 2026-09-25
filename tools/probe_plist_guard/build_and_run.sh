#!/bin/bash
# probe_plist_guard (21t-e, 25 Sep 2026): the EchoJayProbe binary carries an embedded Info.plist that marks it
# background-only, so a sampling pass of 74 launches never touches the Dock.
# The probe is a BARE Mach-O executable, so the plist lives in its __TEXT,__info_plist section - checking the
# source file alone would prove nothing about what shipped.
set -u; cd "$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
fail=0
say() { if [ "$1" = 0 ]; then echo "  ok    $2"; else echo "  FAIL  $2"; fail=1; fi; }

SRC=tools/au_instantiate_probe/EchoJayProbe-Info.plist
[ -f "$SRC" ] && plutil -lint "$SRC" >/dev/null 2>&1; say $? "the source plist exists and parses"
grep -q "LSBackgroundOnly" "$SRC" 2>/dev/null; say $? "...and names LSBackgroundOnly"

# The BUILT binary is the evidence. Build it if no artefact is around.
BIN=""
for c in build-release/EchoJayProbe_artefacts/Release/EchoJayProbe build-guards/EchoJayProbe_artefacts/Release/EchoJayProbe; do
  [ -x "$c" ] && BIN="$c" && break
done
if [ -z "$BIN" ]; then
  echo "  (no probe artefact; building one)"
  cmake --build build-release --target EchoJayProbe -j 4 >/dev/null 2>&1
  BIN=build-release/EchoJayProbe_artefacts/Release/EchoJayProbe
fi
[ -x "$BIN" ]; say $? "a built probe exists  [$BIN]"
if [ -x "$BIN" ]; then
  otool -X -s __TEXT __info_plist "$BIN" > /tmp/ejprobe_plist.$$ 2>/dev/null
  # otool prints the section as hex words; decode to text and look for the key.
  TXT=$(awk '{ for (i = 2; i <= NF; i++) printf "%s", $i }' /tmp/ejprobe_plist.$$ | xxd -r -p 2>/dev/null)
  rm -f /tmp/ejprobe_plist.$$
  echo "$TXT" | grep -q "LSBackgroundOnly"; say $? "the BUILT binary carries LSBackgroundOnly in __TEXT,__info_plist"
  echo "$TXT" | grep -q "com.echojay.EchoJayProbe"; say $? "...and the identifier that names it"
fi
if [ $fail -eq 0 ]; then echo "==== probe_plist_guard: GREEN ===="; exit 0; else echo "==== probe_plist_guard: RED ===="; exit 1; fi
