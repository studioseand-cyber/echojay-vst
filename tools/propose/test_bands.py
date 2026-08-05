#!/usr/bin/env python3
"""Gate for band routing. Run before committing a change to bands.py.

    python3 test_bands.py

THE THREE TRAPS ARE THE ACCEPTANCE TEST, not footnotes. Each is attempted
against the real map on disk, and each must be refused FROM THE EVIDENCE rather
than by a name on a list:

    Dangerous BAX EQ Master     digit run, but the digit is an instance
    DPR-402 (s)                 'L/M' vs 'R/S', one band across two channels
    UAD Manley Massive Passive  'Ch1' vs 'Ch2', same

And both catches are proven LOAD-BEARING: disabling either one lets a trap
through. A catch that never fires is a catch nobody notices losing.
"""
import glob, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bands
from bands import route, tokenise, find_axis, classify_axis, order_bands

MAPS = os.path.expanduser("~/Library/ejmap/maps")
fails = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


def load(name):
    for f in glob.glob(os.path.join(MAPS, "*.json")):
        d = json.load(open(f))
        if d["identity"]["name"] == name:
            return list((d.get("controls") or {}).values())
    raise SystemExit(f"no map for {name} -- this gate reads real maps")


def semantics(name):
    """What stage 2 ACTUALLY proposed for this plugin, read off disk.

    Not a substring approximation written here -- that was the first version of
    this gate and it mislabelled AMEK's HP/LP filters as band frequencies and
    BAX's Low Cut as a band, which refused both for reasons that had nothing to
    do with the traps. Reading the real proposals tests the integration.
    """
    for f in glob.glob(os.path.expanduser("~/Library/ejmap/proposals/*.json")):
        d = json.load(open(f))
        if d["plugin"] != name:
            continue
        out = {p["control_name"]: p["kind"] for p in d["params"]}
        for e in d["escalations"]:
            sems = {a["semantic"] for a in e.get("arms", [])}
            if len(sems) == 1:
                out[e["name"]] = sems.pop()
        return out
    raise SystemExit(f"no proposal for {name} -- run propose.py first")


print("tokeniser")
check(tokenise("Ch1LoMidFreq") == ["CH", "1", "LO", "MID", "FREQ"],
      "camelCase and digit runs split: Ch1LoMidFreq")
check(tokenise("High Shelf Frequency 1") == ["HIGH", "SHELF", "FREQUENCY", "1"],
      "whitespace and a trailing digit split")
check(tokenise("Freq L/M") == ["FREQ", "L/M"],
      "a slash pair stays ONE token -- 'L/M' names one channel")
check(tokenise("LF Freq 2") == ["LF", "FREQ", "2"], "prefix, word, digit")

print("\naxis finding")
check(find_axis(["LF Freq", "LMF Freq", "HF Freq"])[0] == 0, "axis at position 0")
check(find_axis(["LF Freq 2", "LF Gain 2"]) is not None, "one varying position is an axis")
check(find_axis(["LF Freq 2", "LMF Gain 2"]) is None,
      "TWO varying positions is not an axis -- not a band set")
check(find_axis(["Freq", "LF Freq"]) is None, "different token counts cannot share an axis")

print("\naxis classification")
check(classify_axis(["LF", "LMF", "HF"]) == "prefix", "the ordered vocabulary is a prefix axis")
check(classify_axis(["LOW", "MID", "HIGH"]) == "prefix", "spelled-out folds to the vocabulary")
check(classify_axis(["LO", "LOMID", "HIMID", "HI"]) == "prefix", "compound folds too")
check(classify_axis(["1", "2"]) == "digit", "a digit run is a digit axis")
check(classify_axis(["L/M", "R/S"]) == "channel", "a slash pair is a channel marker")
check(classify_axis(["CH1", "CH2"]) == "channel", "Ch<n> is a channel marker")
check(classify_axis(["LOW", "LO"]) is None,
      "two tokens folding to ONE band are not two bands")

# ---------------------------------------------------------------- the traps
print("\nTHE THREE TRAPS, attempted against the real maps")

bax = load("Dangerous BAX EQ Master")
bax_sem = semantics("Dangerous BAX EQ Master")
got, why = route(bax, bax_sem)
check(got is None and "digit axis refused" in (why or "")
      and "indexes the plugin, not a band" in (why or ""),
      "BAX: a digit run REFUSED because the same digit indexes a non-band control")
print(f"         -> {why}")

dpr = load("DPR-402 (s)")
dpr_sem = semantics("DPR-402 (s)")
got, why = route(dpr, dpr_sem)
# Refused before the channel rule is even reached: two freq controls cannot make
# two bands of two members. That is the honest reason and the gate says so rather
# than claiming a catch that did not fire. The channel rule is proven directly,
# on tokens, in the classification block above.
check(got is None and "band-semantic controls" in (why or ""),
      "DPR-402: 'Freq L/M' + 'Freq R/S' REFUSED -- two controls cannot make two bands")
print(f"         -> {why}")

man = load("UAD Manley Massive Passive")
man_sem = semantics("UAD Manley Massive Passive")
got, why = route(man, man_sem)
check(got is None, "Manley: Ch1/Ch2 mixed with the band axis REFUSED")
print(f"         -> {why}")

# ------------------------------------------------ the catches are load-bearing
print("\nboth catches proven LOAD-BEARING (disable one, a trap gets through)")

real_reason = bands.device_axis_reason
bands.device_axis_reason = lambda *a, **k: None          # disable catch 1
got_no_axis, why_no_axis = route(bax, bax_sem)
bands.device_axis_reason = real_reason
check(got_no_axis is None,
      "BAX still refused with the AXIS catch disabled -- the ORDER catch holds it")
print(f"         -> {why_no_axis}")

real_order = bands.order_bands
bands.order_bands = lambda b, c, k: ([{**x, "ordinal": i + 1, "order_evidence": {}}
                                      for i, x in enumerate(b)], None, None)  # disable 2
got_no_order, why_no_order = route(bax, bax_sem)
bands.order_bands = real_order
check(got_no_order is None,
      "BAX still refused with the ORDER catch disabled -- the AXIS catch holds it")
print(f"         -> {why_no_order}")

bands.device_axis_reason = lambda *a, **k: None
bands.order_bands = lambda b, c, k: ([{**x, "ordinal": i + 1, "order_evidence": {}}
                                      for i, x in enumerate(b)], None, None)
got_neither, _ = route(bax, bax_sem)
bands.device_axis_reason = real_reason
bands.order_bands = real_order
check(got_neither is not None,
      "with BOTH catches disabled BAX DOES group -- so both are really doing work")

# ------------------------------------------------------- ordering from sweeps
print("\nordering derives from SWEPT frequencies, never the name")
mk = lambda n, i, lo, hi, unit="hz": {"name": n, "index": i, "unit": unit,
                                      "anchors": [[lo, 0.0], [hi, 1.0]]}
by_name = {c["name"]: c for c in [mk("HF Freq", 1, 2000, 20000), mk("LF Freq", 2, 20, 200)]}
b = [{"label": "HF", "members": {"freq_hz": "HF Freq", "gain_db": "x"}},
     {"label": "LF", "members": {"freq_hz": "LF Freq", "gain_db": "y"}}]
ordered, why, _ = order_bands(b, by_name, "prefix")
check(why is None and [x["label"] for x in ordered] == ["LF", "HF"],
      "ordinals come from measured Hz, not from the order the bands arrived in")
check(ordered[0]["order_evidence"]["from"] == "swept" and
      ordered[0]["order_evidence"]["swept_hz"] == [20, 200],
      "order_evidence records the derivation, so it is auditable not asserted")

nofreq = [{"label": "LF", "members": {"gain_db": "y", "q": "z"}},
          {"label": "HF", "members": {"gain_db": "a", "q": "b"}}]
ordered, why, _ = order_bands(nofreq, by_name, "prefix")
check(ordered is None and "swept frequency" in why,
      "bands with no swept frequency REFUSE rather than falling back to name order")

nounit = {"A": mk("A", 1, 20, 200), "B": mk("B", 2, 4.3, 18.4, unit=None)}
one = [{"label": "LF", "members": {"freq_hz": "A", "gain_db": "x"}},
       {"label": "HF", "members": {"freq_hz": "B", "gain_db": "y"}}]
ordered, why, _ = order_bands(one, nounit, "prefix")
check(ordered is None and "declared hz unit" in why,
      "a frequency whose display declared NO unit supplies no ordinal (the 4K case)")

same = {"A": mk("A", 1, 20, 200), "B": mk("B", 2, 20, 200)}
pair = [{"label": "LF", "members": {"freq_hz": "A", "gain_db": "x"}},
        {"label": "HF", "members": {"freq_hz": "B", "gain_db": "y"}}]
ordered, why, _ = order_bands(pair, same, "digit")
check(ordered is None and "same frequency range" in why,
      "on a DIGIT axis, bands sweeping the SAME range refuse -- the channel case")

# ...but on a PREFIX axis identical ranges are real (AMEK's LF and LMF both sweep
# 15..780 Hz). The grouping stands; the ORDERING is declined. An ordinal is only
# ever written when measurement supports it.
ordered, why, note = order_bands(pair, same, "prefix")
check(why is None and note and "ordering unresolved" in note
      and all(b["ordinal"] is None for b in ordered),
      "on a PREFIX axis identical ranges leave ordinals UNSET rather than guessing")

# ------------------------------------------------------------- the real sets
print("\nthe genuine band sets group")
for name, expect, ordering in [
    ("API-550A (m)", 3, "swept"),
    # LF and LMF both sweep 15..780 Hz, HMF and HF both 370..26000 Hz. Real
    # console behaviour, so the grouping stands and the ordinals do not.
    ("AMEK EQ 200", 5, "unresolved"),
    # Four band gains, and of its band frequencies only LF and LMF declare hz --
    # HF Freq declares no unit and sweeps 4.3..18.4, which is kHz by inference
    # and therefore not orderable. Those two order; the rest are unassigned.
    ("UAD 4K Channel Strip", 2, "swept"),
]:
    ctrls = load(name)
    sem = semantics(name)
    got, why = route(ctrls, sem)
    if not got:
        check(False, f"{name}: expected {expect} bands, refused -- {why}")
        continue
    labels = [b["label"] for b in got["bands"]]
    check(len(got["bands"]) == expect and got["ordering"] == ordering,
          f"{name}: {len(got['bands'])} bands, ordering {got['ordering']} {labels}")
    if got["ordering"] == "swept":
        hz = [b["order_evidence"]["sort_key"] for b in got["bands"]]
        check(hz == sorted(hz) and all(b["ordinal"] for b in got["bands"]),
              f"{name}: ordinals ascend by measured Hz {[round(h) for h in hz]}")
    else:
        check(all(b["ordinal"] is None for b in got["bands"]),
              f"{name}: no ordinal is asserted where measurement cannot support one")

print("\ncorpus sweep -- every map, nothing crashes, refusals are stated")
grouped = refused = 0
for f in sorted(glob.glob(os.path.join(MAPS, "*.json"))):
    d = json.load(open(f))
    ctrls = list((d.get("controls") or {}).values())
    try:
        sem = semantics(d["identity"]["name"])
    except SystemExit:
        continue
    got, why = route(ctrls, sem)
    if got:
        grouped += 1
        print(f"    GROUP  {len(got['bands'])} bands ({got['ordering']:10s}) "
              f"{d['identity']['name']}")
    else:
        refused += 1
        check(bool(why), f"{d['identity']['name']}: a refusal states its reason")
print(f"    -> {grouped} grouped, {refused} refused, over {grouped+refused} maps")

print()
if fails:
    print(f"{len(fails)} FAILURES")
    for f in fails:
        print("   " + f)
    sys.exit(1)
print("all green")
