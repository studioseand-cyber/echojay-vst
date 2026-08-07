#!/usr/bin/env python3
"""Gate for stage 0.5. Calls no API: everything here is the mechanical half --
the collapse, the sibling inheritance, the mark key, and the four dispositions.

    python3 test_categorise.py

The API half has its own acceptance test, `categorise.py --selftest`, which
re-runs the 40 hand-categorised maps. It costs money, so it is explicit.
"""
import os, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from categorise import (base_name, product_key, mark_product_key, scan_products,
                        decide, merge, CATEGORIES)

fails = checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print(f"FAIL: {what}")


# --------------------------------------------------------------------------
# the collapse. 1,819 rows are 1,073 products and paying twice to be told a
# (m)/(s) pair has one category is waste.
check(base_name("CLA-76 (m)") == "CLA-76", "(m) is a variant suffix")
check(base_name("CLA-76 (s)") == "CLA-76", "(s) too")
check(base_name("B360 (6->4)") == "B360", "an NxN channel form is a variant")
check(base_name("WLM Plus (6->6)") == "WLM Plus", "...whatever the channel counts")
check(base_name("PuigChild 660") == "PuigChild 660", "a trailing number is NOT a variant")
check(base_name("API-2500") == "API-2500", "nor a hyphenated model number")
check(base_name("Scheps Omni Channel") == "Scheps Omni Channel", "nothing to strip")

# A NEGATIVE that matters: the suffix must be a whole parenthesised token at the
# END. "Mix (m) Bus" is not a variant of "Mix Bus", and a substring rule would
# say it was.
check(base_name("Mix (m) Bus") == "Mix (m) Bus", "a mid-name (m) is not a variant suffix")

check(product_key("CLA-76 (m)", "Waves") == product_key("CLA-76 (s)", "Waves"),
      "the mono and stereo forms collapse to ONE product")
check(product_key("CLA-76", "Waves") != product_key("CLA-76", "Softube"),
      "...but the same name from a different vendor does not")

# --------------------------------------------------------------------------
# the mark key. unmappable is keyed format|hex(uniqueId) with the version
# DROPPED, because it is a decision about a product, carried across updates.
check(mark_product_key("AudioUnit", "70194649") == "AudioUnit|70194649",
      "the scan attribute IS the hex the C++ key uses -- converting it is the bug")
check(mark_product_key("AudioUnit", "26a5d6d") == "AudioUnit|26a5d6d",
      "and a value with a letter in it proves the point (this one crashed a "
      "decimal reading on the first real run)")
check(mark_product_key("VST3", "0") == "VST3|0", "a zero uid is still a key")
check(mark_product_key("AudioUnit", "0042F15D9") == "AudioUnit|42f15d9",
      "leading zeros and case are normalised, as toHexString emits neither")

# --------------------------------------------------------------------------
# the four dispositions. The gate can only REFUSE.
hi = lambda c: {"category": c, "confidence": "high"}
lo = lambda c: {"category": c, "confidence": "low"}

d, cat, why = decide(hi("eq"), hi("eq"))
check((d, cat) == ("sweep", "eq") and why == "", "agreed and confident -> sweep")

d, cat, why = decide(lo("eq"), hi("eq"))
check((d, cat) == ("sweep", "eq"), "agreed but hedged -> STILL swept")
check("hedged" in why, "...and the row says it was hedged, so the pile is identifiable")

d, cat, _ = decide(hi("eq"), hi("compressor"))
check((d, cat) == ("review", None), "disagreement -> review")

d, cat, _ = decide(hi("none"), hi("none"))
check((d, cat) == ("no_dial_set", None), "agreed none -> no_dial_set")

d, cat, _ = decide(lo("none"), hi("none"))
check(d == "no_dial_set", "a hedged none is still no_dial_set: it does not get loaded either way")

d, cat, _ = decide(hi("not_a_processor"), hi("not_a_processor"))
check((d, cat) == ("not_a_processor", None), "agreed not_a_processor -> its own state")

# THE RULE THIS FILE EXISTS FOR. The two refusals are not one bucket: `none`
# says the ELEVEN CATEGORIES do not cover a real processor, `not_a_processor`
# says there is nothing to dial. Collapsing them would mark ~293 products
# unmappable -- carried forward across every future version -- for a gap in our
# vocabulary rather than a fact about the plugin.
check(decide(hi("none"), hi("none"))[0] != decide(hi("not_a_processor"),
                                                  hi("not_a_processor"))[0],
      "none and not_a_processor MUST reach different dispositions")
check(decide(hi("none"), hi("not_a_processor"))[0] == "review",
      "and one arm of each is a disagreement, not a refusal")

# ...but a disagreement BETWEEN TWO REFUSALS does not block the sweep, because
# the sweep never opens the plugin either way. Which refusal it is matters only
# to the unmappable review. 14 of the 87 real disagreements are this shape.
from categorise import sweepable
_, _, why = decide(hi("none"), hi("not_a_processor"))
check("both refused" in why, "a two-refusal disagreement says so in its reason")
check(not sweepable(merge({"name":"Z","vendor":"V","formats":[],"declared":"",
                           "members":[],"mark_keys":[]},
                          hi("none"), hi("not_a_processor"), "t")),
      "and the sweep SKIPS it rather than waiting on a decision it does not need")
check(sweepable(merge({"name":"Z","vendor":"V","formats":[],"declared":"",
                       "members":[],"mark_keys":[]}, lo("eq"), hi("eq"), "t")),
      "while a hedged agreement IS swept")
check(not sweepable(merge({"name":"Z","vendor":"V","formats":[],"declared":"",
                           "members":[],"mark_keys":[]}, hi("eq"), hi("delay"), "t")),
      "and a real disagreement is not swept: there is no category to sweep with")

d, _, why = decide({"category": None}, hi("eq"))
check(d == "review" and "nothing" in why, "a missing answer is not a disagreement")

d, _, why = decide(hi("dynamics"), hi("dynamics"))
check(d == "review", "a confident agreement on a word that is not a category is refused")

# --------------------------------------------------------------------------
# merge carries what the review pile needs to be identifiable
m = merge({"name": "X", "vendor": "V", "formats": ["AudioUnit"], "declared": "",
           "members": ["AudioUnit:x"], "mark_keys": ["AudioUnit|1"]},
          lo("eq"), hi("eq"), "2026-08-05T00:00:00")
check(m["disposition"] == "sweep" and m["confidence"] == "hedged",
      "a hedged sweep records BOTH that it sweeps and that it hedged")
check(len(m["arms"]) == 2 and m["arms"][0]["confidence"] == "low",
      "both arms' answers are kept, so a decision can be re-derived")

m = merge({"name": "Y", "vendor": "V", "formats": ["VST3"], "declared": "",
           "members": [], "mark_keys": []},
          {"category": "none", "confidence": "high", "kind": "Stereo Imager"},
          {"category": "none", "confidence": "high", "kind": "stereo imager"},
          "2026-08-05T00:00:00")
check(m["kind_agreed"], "kind agreement is case- and space-insensitive")
check(m["kind"] == "Stereo Imager", "and the kind is kept for the ranked grouping")

# --------------------------------------------------------------------------
# scan_products against a synthetic cache: the sibling inheritance is the one
# piece of free evidence in the whole pass and it is worth 493 rows.
with tempfile.TemporaryDirectory() as tmp:
    open(os.path.join(tmp, "scan-cache.xml"), "w").write("""<EJMAP_SCAN_CACHE>
 <PLUGIN name="Widget (m)" manufacturer="Acme" category="" uniqueId="17"
         file="AudioUnit:Effects/aufx,widg,Acme"/>
 <PLUGIN name="Widget (s)" manufacturer="Acme" category="" uniqueId="18"
         file="AudioUnit:Effects/aufx,widh,Acme"/>
 <PLUGIN name="Widget" manufacturer="Acme" category="Fx|Dynamics" uniqueId="19"
         file="/Library/Audio/Plug-Ins/VST3/Widget.vst3"/>
 <PLUGIN name="Orphan" manufacturer="Acme" category="" uniqueId="20"
         file="AudioUnit:Effects/aufx,orph,Acme"/>
</EJMAP_SCAN_CACHE>""")
    ps = scan_products(tmp)
    check(len(ps) == 2, f"4 rows collapse to 2 products (got {len(ps)})")
    w = ps[product_key("Widget", "Acme")]
    check(len(w["members"]) == 3, "all three binaries are members of one product")
    check(w["declared"] == "Fx|Dynamics",
          "THE AU ROWS INHERIT THE VST3 SIBLING'S SUBCATEGORY (no AU declares one)")
    check(sorted(w["formats"]) == ["AudioUnit", "VST3"], "both formats recorded")
    check(len(w["mark_keys"]) == 3,
          "each binary keeps its OWN mark key: a mark is per product-id, and the "
          "AU and VST3 are different products to marks.json")
    check(ps[product_key("Orphan", "Acme")]["declared"] == "",
          "a product with no sibling inherits nothing, and claims nothing")

# --------------------------------------------------------------------------
# CHECK 2's SIGNATURE INCLUDES THE INDEX, and that is what separates a sibling
# from a leak. Sibilance (s) and Sibilance-Live (m) share five control names,
# ranges and anchor counts exactly -- same processor, two builds -- but (s) has
# a Lookahead at index 0, so its five sit at 1-5 and (m)'s at 0-4. A LEAK copies
# the donor's indices verbatim; a sibling's are its own.
from audit_maps import control_signature

def mk(pairs):
    return {"controls": {n: {"index": i, "range": [0, 1], "unit": None, "anchors": [1, 2]}
                         for n, i in pairs}}

names = ["Detection", "Threshold", "Range", "Mode", "Monitor"]
sib_m = mk(list(zip(names, range(0, 5))))
sib_s = mk(list(zip(names, range(1, 6))))
check(control_signature(sib_m) != control_signature(sib_s),
      "audit: SIBLINGS with the same names at DIFFERENT indices are distinguishable")
check(control_signature(sib_m) == control_signature(mk(list(zip(names, range(0, 5))))),
      "audit: ...while a true copy -- indices included -- still matches, which is "
      "what a leak looks like")

print(f"{checks} checks, {fails} failures")
sys.exit(1 if fails else 0)
