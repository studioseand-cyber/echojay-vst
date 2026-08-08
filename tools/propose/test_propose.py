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

# The confidence gate was DROPPED 7 Aug 2026 -- hedged agreements matched the
# hold-out 42/43 against 74/75 for confident ones, and the old gate was
# manufacturing 34% of the review pile to buy a point inside the noise. This
# assertion used to demand the opposite and was left red by that change; it now
# asserts what was measured, so a silent re-introduction of the gate fails here.
acc, esc, dec = merge([ev("Gain")], [A("Gain", "output_db", "low")], [A("Gain", "output_db", "high")])
check(len(acc) == 1 and acc[0]["hedged"] is True and not esc,
      "a HEDGED agreement is still an agreement -- accepted, and recorded as hedged")

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

print("\nchunking -- splits the QUESTION, never the judgement")

# Both properties are tested through run_one rather than merge, because both are
# claims about the CALLER: that it merges once over the whole, and that it
# writes nothing when a piece is missing. Testing merge alone would prove
# neither.
import shutil, tempfile
import propose as P

_chunks_asked = []


def _fake_arm(answers, fail_on=None):
    """An arm that answers only the controls it was shown, so a test that
    accidentally sent the whole plugin in one request would look different.

    `fail_on` is a CONTROL NAME, not a call count: a truncation is a property of
    the chunk, so it must recur on every retry of that chunk the way the real
    one did. Failing the Nth call instead would be cured by the backoff and the
    test would pass for the wrong reason.
    """
    def arm(_client, user, *a):
        shown = re.findall(r"name='([^']*)'", user)
        _chunks_asked.append(shown)
        if fail_on is not None and fail_on in shown:
            raise RuntimeError("Expecting ',' delimiter: line 1 column 32768")
        return [{"name": n, **answers[n]} for n in shown if n in answers]
    return arm


def _map_of(names_units):
    ctrls = {n: {"name": n, "index": i, "kind": "anchored", "range": [0, 1],
                 "unit": u, "anchors": [[0.0, 0.0], [1.0, 1.0]]}
             for i, (n, u) in enumerate(names_units)}
    return {"fp": "TESTFP", "identity": {"name": "Giant"}, "category": "eq",
            "params": {}, "controls": ctrls}


def _run(doc, chunk, fail_on=None, answers=None):
    """run_one against stubbed arms, in a scratch dir. Returns (note, files)."""
    tmp = tempfile.mkdtemp()
    try:
        mp = os.path.join(tmp, "m.json")
        json.dump(doc, open(mp, "w"))
        out = os.path.join(tmp, "out")
        os.mkdir(out)
        old = (P.ask_opus, P.ask_gpt, P.time.sleep)
        _chunks_asked.clear()
        P.ask_opus = _fake_arm(answers, fail_on)
        P.ask_gpt = lambda c, u: _fake_arm(answers)(c, u)
        P.time.sleep = lambda *_: None          # the backoff, not the test's job
        try:
            note = P.run_one(mp, out, {"anthropic": None, "openai": None},
                             False, "RUN", "high", chunk)
        finally:
            P.ask_opus, P.ask_gpt, P.time.sleep = old
        files = os.listdir(out)
        doc_out = json.load(open(os.path.join(out, files[0]))) if files else None
        return note, files, doc_out
    finally:
        shutil.rmtree(tmp)


# 1. GATE 4 ACROSS A BOUNDARY. Two controls claim gain_db; the chunk size puts
#    them in DIFFERENT requests. Per-chunk merging would accept both, and the
#    second would silently overwrite the first in a dict keyed by semantic.
doc = _map_of([("A", "db"), ("B", "hz"), ("C", "db"), ("D", "hz")])
ans = {"A": {"semantic": "gain_db", "confidence": "high"},
       "B": {"semantic": "freq_hz", "confidence": "high"},
       "C": {"semantic": "gain_db", "confidence": "high"},
       "D": {"semantic": "none", "confidence": "high"}}
note, files, out = _run(doc, chunk=2, answers=ans)
check([len(c) for c in _chunks_asked[::2]] == [2, 2], "4 controls at chunk=2 became 2 requests")
check(len(out["params"]) == 1 and out["params"][0]["kind"] == "freq_hz",
      "only the unduplicated semantic is accepted")
claims = [e["why"] for e in out["escalations"] if "claim gain_db" in e["why"]]
check(len(claims) == 2,
      "GATE 4 STILL FIRES ACROSS THE BOUNDARY: both gain_db claimants escalate")
check(out["run"]["chunking"] == {"chunks": 2, "controls_per_chunk": 2},
      "the proposal records that it was asked in chunks, so it is not mistaken "
      "for a whole-surface answer")

# 2. A FAILED CHUNK LEAVES NOTHING. The first chunk answers fine; the second
#    truncates. A partial file would look complete forever.
note, files, out = _run(doc, chunk=2, fail_on="C", answers=ans)
check(files == [], "a failed chunk writes NO proposal file at all")
check("chunk 2/2" in note[3] and "whole plugin" in note[3],
      f"and the note names the chunk that failed: {note[3][:60]!r}")

# 3. The un-chunked path is unchanged: one request, no chunking record.
note, files, out = _run(doc, chunk=0, answers=ans)
check(len(_chunks_asked[0]) == 4, "chunk=0 asks for the whole plugin in one request")
check("chunking" not in out["run"],
      "an unchunked proposal carries no chunking field -- absence means whole-surface")

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
