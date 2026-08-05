#!/usr/bin/env python3
"""Gate for the propose tool. Run before committing a change to it.

    python3 test_propose.py
"""
import json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evidence import semantic_unit, unit_family_conflict, candidates, control_evidence
from propose import merge

CPP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "..", "Source", "EchoJayParamApply.h")

fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


print("unit family, against the C++ it mirrors")

# The copy is ASSERTED, not trusted: read semanticUnit out of the header and
# compare every mapping. A rule that exists twice is two rules.
src = open(CPP).read()
body = re.search(r"inline juce::String semanticUnit[^{]*\{(.*?)\n\}", src, re.S).group(1)
pairs = re.findall(r'key == "(\w+)"\)\s*return "(\w+)"', body)
suffixes = re.findall(r'key\.endsWith \("(\w+)"\)\)\s*return "(\w+)"', body)
check(pairs == [("ratio", "ratio")], f"C++ exact-match rules are {pairs}")
for suffix, family in suffixes:
    check(semantic_unit("x" + suffix) == family, f"suffix {suffix} -> {family}")
check(len(suffixes) == 5, f"C++ declares {len(suffixes)} suffix rules, python mirrors 5")
check(semantic_unit("drive") == "", "drive makes no unit claim")

print("\nunit family, the rule's own behaviour")
check(unit_family_conflict("attack_ms", "db") is not None,
      "ms semantic on a dB display refuses (the Transient Designer case)")
check(unit_family_conflict("output_db", "pct") is not None,
      "dB semantic on a % display refuses (the HG-2 case)")
check(unit_family_conflict("output_db", "db") is None, "matching units raise nothing")
check(unit_family_conflict("drive", "db") is None, "a semantic with no claim raises nothing")
check(unit_family_conflict("output_db", "") is None, "an undeclared display raises nothing")
check(unit_family_conflict("output_db", None) is None, "a null display raises nothing")

print("\nthe merge -- every gate refuses, and only refuses")
ev = lambda n, u="": {"name": n, "index": 1, "kind": "anchored", "range": None,
                      "unit": u, "anchors": 21, "span": [-10, 10]}
A = lambda n, s, c: {"name": n, "semantic": s, "confidence": c}

acc, esc, dec = merge([ev("Gain")], [A("Gain", "output_db", "high")], [A("Gain", "output_db", "high")])
check(len(acc) == 1 and acc[0]["semantic"] == "output_db", "agree + confident accepts")
check(acc[0]["arms"][0]["model"] != acc[0]["arms"][1]["model"], "both arms recorded")

acc, esc, dec = merge([ev("Gain")], [A("Gain", "output_db", "high")], [A("Gain", "makeup_db", "high")])
check(not acc and "disagree" in esc[0]["why"], "disagreement escalates")

acc, esc, dec = merge([ev("Gain")], [A("Gain", "output_db", "low")], [A("Gain", "output_db", "high")])
check(not acc and "confident" in esc[0]["why"], "a declining arm escalates")

acc, esc, dec = merge([ev("Gain")], [A("Gain", "none", "high")], [A("Gain", "none", "high")])
check(not acc and not esc and len(dec) == 1,
      "a CONFIDENT agreed 'none' is a decline, not a row for the human")
acc, esc, dec = merge([ev("Gain")], [A("Gain", "none", "low")], [A("Gain", "none", "high")])
check(not acc and not esc and len(dec) == 1 and dec[0]["confident"] is False,
      "a HEDGED agreed 'none' also declines, flagged not-confident (57/57 in the pile)")
check("hedged" in dec[0]["why"], "and the record says an arm hedged, so it can be re-opened")

acc, esc, dec = merge([ev("Attack", "db")], [A("Attack", "attack_ms", "high")],
                 [A("Attack", "attack_ms", "high")])
check(not acc and "unit family" in esc[0]["why"],
      "unit family refuses what both arms confidently agreed (the backstop)")

acc, esc, dec = merge([ev("Gain")], [A("Gain", "not_a_semantic", "high")],
                 [A("Gain", "not_a_semantic", "high")])
check(not acc and "vocabulary" in esc[0]["why"], "an invented semantic escalates")

acc, esc, dec = merge([ev("Gain")], [], [A("Gain", "output_db", "high")])
check(not acc and "did not answer" in esc[0]["why"], "a missing answer escalates")

two = [ev("Out A"), {**ev("Out B"), "index": 2}]
both = [A("Out A", "output_db", "high"), A("Out B", "output_db", "high")]
acc, esc, dec = merge(two, both, both)
check(not acc and len(esc) == 2 and "2 controls claim output_db" in esc[0]["why"],
      "two controls claiming one semantic escalates BOTH (params is a dict)")

print("\ncandidates -- a human answer is never re-litigated")
doc = {"identity": {"name": "x"}, "category": "compressor",
       "params": {"threshold_db": {"index": 5, "name": "Thresh", "kind": "anchored",
                                   "range": None, "unit": "db", "anchors": []}},
       "controls": {"Thresh": {"index": 5, "name": "Thresh", "kind": "anchored",
                               "range": None, "unit": "db"},
                    "Mix": {"index": 9, "name": "Mix", "kind": "anchored",
                            "range": [0, 100], "unit": "pct"}}}
rows = candidates(doc)
check([r["name"] for r in rows] == ["Mix"], "a control already claimed by a param is skipped")
check([r["name"] for r in candidates(doc, audit=True)] == ["Thresh"],
      "audit mode proposes over the confirmed params instead")

print("\nevidence shape is frozen")
c = control_evidence({"name": "n", "index": 0, "kind": "anchored", "range": [0, 1],
                      "unit": "db", "anchors": [[-6.0, 0.0], [6.0, 1.0]],
                      "labels": {"A": 0.0}})
check(set(c) == {"name", "index", "kind", "range", "unit", "anchors", "span"},
      "exactly the seven fields the hold-out measured against")
check(c["span"] == [-6.0, 6.0], "span is derived from the anchor table")
check("labels" not in c, "mode labels are NOT sent (the hold-out did not have them)")

print()
if fails:
    print(f"{len(fails)} FAILURES")
    for f in fails:
        print("   " + f)
    sys.exit(1)
print("all green")
