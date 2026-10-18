#!/bin/bash
# EJ Map - the two accuracy-gap steps of the 16 Oct re-check, corrected (Kathy, 18 Oct). ONE command, from inside the 16 Oct pack folder:
#     bash ~/Downloads/SEAN_RECHECK_PACK_16oct/recheck_gaps_18oct.sh        (copy this file into that folder first)
# Why: the pack's run-all steps passed "--redo accuracy_gaps", which this build (ad50ddbb) refuses ("not a Phase B category", exit 2).
# The same work, in the values ad50ddbb accepts:
#   gain-cal + timing: --redo unfinished   (for these two categories accuracy_gaps IS unfinished; missing rows run anyway)
#   combined + material + frequency: --redo nothing_nominated   (missing rows run because the categories are named; on your cert 6 folder
#                                     no combined row composed nothing, so this runs exactly the missing rows)
# Expected from your cert 6: gain-cal + timing for AMEK Mastering Compressor, Maag MAGNUM-K (missing) and Auto-Tune Vocal Compressor
# (timed out); combined / material / frequency for AMEK, Maag, VBC FG-Grey, VBC FG-MU, VBC Rack (missing). About 1.5-2 h
# (Auto-Tune VC's timing alone was ~60 min). Then the drafts. Nothing is sent.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; BIN="$HERE/ejmap.app/Contents/MacOS/ejmap"; CERT="$HOME/Library/ejmap/cert"; LOG="$CERT/recheck_18oct.log"
[ -x "$BIN" ] || { echo "GAPS: no ejmap.app in $HERE - put this file in the SEAN_RECHECK_PACK_16oct folder"; exit 2; }
[ -d "$CERT" ] || { echo "GAPS: no cert folder at $CERT"; exit 2; }
ONLY=(); while IFS= read -r n; do [ -n "$n" ] && ONLY+=(--only "$n"); done < "$HERE/held_units.txt"
echo "GAPS: $(( ${#ONLY[@]} / 2 )) units; log $LOG" | tee -a "$LOG"
"$BIN" --cert-preflight || { echo "GAPS: the preflight refused - nothing was run"; exit 3; }
caffeinate -dimsu "$BIN" --phaseb-all --redo unfinished --category gaincal --category timing "${ONLY[@]}" --out "$CERT" --jobs 2 2>&1 | tee -a "$LOG"
caffeinate -dimsu "$BIN" --phaseb-all --redo nothing_nominated --category combined --category material --category frequency "${ONLY[@]}" --out "$CERT" --jobs 2 2>&1 | tee -a "$LOG"
"$BIN" --phaseb-drafts --out "$CERT" 2>&1 | tee -a "$LOG"
echo "GAPS: done. Zip ~/Library/ejmap/cert and send it to Kathy." | tee -a "$LOG"
