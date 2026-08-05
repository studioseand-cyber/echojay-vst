#!/usr/bin/env python3
"""Gate for cohort detection, and for the channel rule in particular.

THE CHANNEL RULE IS PROVEN ON ALL THREE NAMING SHAPES THE CORPUS USES, because
a rule that catches two of three is worse than one that catches none: it looks
like it works.

    DPR-402      'Freq L/M'   vs 'Freq R/S'       slash pairs
    PuigChild    'Left Input' vs 'Right Input'    spelled out
    Vertigo      'OutGnL'     vs 'OutGnR'         single letter, compounded
    Manley       'Ch1HiBW'    vs 'Ch2HiBW'        a digit under a CH prefix
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bands import tokenise, find_axis, classify_axis

fails = []
def check(c, w):
    print(("  ok   " if c else "  FAIL ") + w)
    if not c: fails.append(w)

def klass(names):
    ax = find_axis(sorted(names))
    if not ax: return None
    pos, toks = ax
    first = tokenise(sorted(names)[0])
    return classify_axis(toks, first[:pos] + first[pos + 1:])

print("the four channel naming shapes -- ALL of them, or the rule is a liability")
for who, names in [
    ("DPR-402 slash pair",   ["Freq L/M", "Freq R/S"]),
    ("DPR-402, longer base", ["Peak Lim Attack L/M", "Peak Lim Attack R/S"]),
    ("PuigChild spelled",    ["Left Input", "Right Input"]),
    ("PuigChild, 2 words",   ["Left Threshold", "Right Threshold"]),
    ("Vertigo compounded",   ["OutGnL", "OutGnR"]),
    ("Manley CH + digit",    ["Ch1HiBW", "Ch2HiBW"]),
    ("Ampeg CH + digit",     ["Ch64Vol", "Ch66Vol"]),
    ("mid/side",             ["Mid Gain", "Side Gain"]),
]:
    check(klass(names) == "channel", f"{who}: {names} -> channel")

print("\nand the negatives, which is where a set-based rule earns its keep")
check(klass(["Low Freq", "Mid Freq", "High Freq"]) == "prefix",
      "Low/Mid/High is a BAND set -- 'mid' is only a channel next to 'side'")
check(klass(["LF Freq", "LMF Freq", "HMF Freq"]) == "prefix", "LF/LMF/HMF is a band set")
check(klass(["High Shelf Level 1", "High Shelf Level 2"]) == "digit",
      "a digit with no CH beside it stays an instance digit, not a channel")
check(klass(["CMP Release", "EXP Release"]) is None,
      "CMP/EXP is neither -- genuinely distinct controls (queue 17)")
check(klass(["Output Level 1", "Output Level 2"]) == "digit",
      "BAX instance numbers are not channels")

print("\ncohort detection over a real map set")
import json, glob, review
G = os.environ.get("GATEROOT")
if G and os.path.isdir(G):
    pile, _ = review.load_pile(G, None)
    ch = [r for r in pile if r["kind"] == "cohort"
          and r["cohort"]["axis_kind"] == "channel"]
    plugs = {r["plugin"].split(" (")[0] for r in ch}
    for want in ("DPR-402", "PuigChild", "UAD Vertigo VSM-3"):
        check(any(want in p for p in plugs),
              f"{want} produces at least one CHANNEL cohort")
    covered = sum(len(r["covers"]) for r in pile if r["kind"] in ("cohort", "band_set"))
    sets = sum(1 for r in pile if r["kind"] in ("cohort", "band_set"))
    check(covered > sets, f"{sets} set questions stand in for {covered} rows")
else:
    print("  (set GATEROOT to a scratch ejmap root to run the corpus half)")

print()
if fails:
    print(f"{len(fails)} FAILURES"); [print("   " + f) for f in fails]; sys.exit(1)
print("all green")
