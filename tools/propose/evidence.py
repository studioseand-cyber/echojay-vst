"""The evidence a proposal is allowed to see, and the rules that judge one.

THE EVIDENCE SHAPE IS FROZEN ON PURPOSE. It is exactly what the 4 Aug hold-out
measured 98.7% auto-accept precision against: name, kind, declared range,
QUALIFIED 7 Aug 2026: the hand answers this is measured against are
PARTLY MODEL-SOURCED -- some of the 266 review decisions came from
pasting cards into a third model, so the standard is not fully
independent of what it scores. The DIRECTION holds: both errors were
adjudicated by the unit-family rule over measured evidence.
MEASURED display unit, anchor count, and the span the sweep observed. Nothing
else -- notably not a mode control's labels, which would probably help identify
`position` and which the hold-out did not have. Widening the evidence invalidates
the number, so widening it means measuring again first.
"""
import json

# The union of every category's dial set (EjmapAssignment.h DialSets), plus
# `position`, which the corpus carries for stepped selectors.
VOCAB = [
    "threshold_db", "ratio", "attack_ms", "release_ms", "makeup_db", "knee_db",
    "mix_pct", "wet_pct", "input_db", "output_db", "ceiling_db",
    "freq_hz", "gain_db", "q", "low_cut_freq_hz", "high_cut_freq_hz",
    "sensitivity", "drive", "delay_time_ms", "feedback_pct",
    "reverb_decay_s", "predelay_ms", "position",

    # Added 8 Aug 2026 from the 1,095-map corpus -- 1,235 controls that both
    # arms called `none` because there was no word for them. Breadth was the
    # test: every one appears on 21+ plugins. `speed` passed breadth (34
    # plugins) and was HELD BACK because it is two semantics wearing one word,
    # a tape percentage and a detector time constant, which no unit check can
    # separate. docs/VOCABULARY_ADDITIONS.md carries the counts and the cap.
    #
    # THE SUFFIX IS A CLAIM, not a label: unitFamilyConflicts checks it. So
    # slope_db_oct and tempo_bpm deliberately end in families semanticUnit does
    # NOT know, and therefore claim nothing -- inventing dB-per-octave as a unit
    # family would change the rule the hold-out was measured against.
    "range_db", "depth_pct", "reverb_size", "width_pct", "mod_rate_hz",
    "tone", "balance", "slope_db_oct", "hold_ms", "density", "tempo_bpm", "pan",
]


def semantic_unit(key):
    """Mirrors echojay::semanticUnit in Source/EchoJayParamApply.h.

    Kept in step by test_unit_family_matches_cpp, which reads the C++ and
    compares -- a rule that exists twice is two rules, so the copy is asserted
    rather than trusted.
    """
    if key == "ratio":
        return "ratio"
    for suffix, family in (("_db", "db"), ("_ms", "ms"), ("_hz", "hz"),
                           ("_pct", "pct"), ("_s", "s")):
        if key.endswith(suffix):
            return family
    return ""


def unit_family_conflict(semantic, measured_unit):
    """Mirrors ejmap::unitFamilyConflicts. An absent unit on EITHER side claims
    nothing: a semantic with no suffix (drive, sensitivity) makes no claim, and
    a display that declared no unit was MEASURED as declaring none.

    This is the backstop that makes model-proposed semantics safe. Over the
    hold-out's 125 rows it fired exactly twice, and both were real.
    """
    want = semantic_unit(semantic)
    got = (measured_unit or "").strip()
    if not want or not got or want == got:
        return None
    return (f"{semantic} expects '{want}' but the sweep measured '{got}' "
            f"- the name and the behaviour disagree")


def unappliable_conflict(semantic, ev):
    """Can this semantic actually be WRITTEN to this control? Accept-path only.

    `interpolateAnchors` (EchoJayParamApply.h:357) opens with
    `if (anchors.isEmpty()) return 0.0f;` -- so a semantic with no anchor table
    lands at normalized ZERO whatever value is asked for. Not a wrong value: the
    same wrong value every time, silently, with the write reported as applied.

    `position` is exempt and must stay exempt. It is set by STEP INDEX, not by
    interpolation, so an empty anchor table is its normal condition -- 17 of the
    23 zero-anchor accepts in the 8 Aug corpus are `position` on a mode control
    and every one of them is correct.

    TWO CLAUSES, RECORDED SEPARATELY, because they are different kinds of claim:

      unappliable   0 anchors + a non-position semantic. PROVABLE from the C++:
                    the write cannot express the value. 5 rows corpus-wide, all
                    real -- UAD Neve 31102/31102SE `HiQ` -> q (a high-Q SWITCH),
                    and Quantum's two Vibrato controls.
      stepped       a `mode`-kind control + a non-position semantic. A judgement,
                    not a proof. It adds ZERO rows beyond the first clause today
                    (all 3 mode-kind offenders have empty tables), so it is a
                    guard rather than a filter -- if it ever fires on a control
                    with a real anchor table, score its discards before trusting
                    it.

    THE BROAD VERSION OF THIS RULE WAS MEASURED AND REJECTED: "<=3 anchors + a
    continuous semantic" flags 58 accepts (1.39%) and most are CORRECT -- a
    3-position frequency selector really is a frequency, a 3-step Ratio switch
    really is a ratio. Scoring its discards killed it, the same way scoring the
    confidence gate's discards killed that one. Few anchors is an interpolation
    PRECISION concern; no anchors is a wrongness one.
    """
    if semantic == "position":
        return None
    if (ev.get("anchors") or 0) == 0:
        return ("unappliable: no anchor table, so interpolateAnchors returns 0.0 "
                "for every value asked -- this control cannot be dialled at all")
    if ev.get("kind") == "mode":
        return ("stepped: a mode control carrying a continuous semantic; "
                "position is the semantic for a stepped selector")
    return None


def control_evidence(ctrl):
    """One control, reduced to the frozen evidence shape."""
    anchors = ctrl.get("anchors") or []
    span = None
    if anchors:
        vals = [a[0] for a in anchors if isinstance(a, list) and a]
        if vals:
            span = [min(vals), max(vals)]
    return {
        "name": ctrl.get("name"),
        "index": ctrl.get("index"),
        "kind": ctrl.get("kind"),
        "range": ctrl.get("range"),
        "unit": ctrl.get("unit") or "",
        "anchors": len(anchors),
        "span": span,
    }


def candidates(map_doc, audit=False):
    """Which controls this map wants proposed.

    Default: every control the map exposes that no Tier 1 param already claims,
    by INDEX. A human answer is never re-litigated.

    audit=True inverts it -- propose over the already-confirmed params instead,
    so the tool can be scored against a human answer. That is the self-test, not
    the production path.
    """
    controls = map_doc.get("controls") or {}
    params = map_doc.get("params") or {}
    claimed = {p.get("index") for p in params.values() if isinstance(p, dict)}

    if audit:
        out = []
        for semantic, p in params.items():
            if not isinstance(p, dict):
                continue
            ev = control_evidence(p)
            ev["name"] = p.get("name")
            ev["truth"] = semantic
            out.append(ev)
        return out

    return [control_evidence(c) for c in controls.values()
            if c.get("index") not in claimed]


def format_for_prompt(map_doc, rows):
    lines = [f"plugin: {map_doc['identity']['name']}",
             f"category: {map_doc.get('category') or 'unknown'}",
             "", "controls:"]
    for c in rows:
        rng = "null" if not c["range"] else f"{c['range'][0]:g}..{c['range'][1]:g}"
        spn = "null" if not c["span"] else f"{c['span'][0]:g}..{c['span'][1]:g}"
        lines.append(
            f"  name={c['name']!r}  kind={c['kind']}  range={rng}  span={spn}  "
            f"unit={c['unit'] or '(none declared)'}  anchors={c['anchors']}")
    return "\n".join(lines)
