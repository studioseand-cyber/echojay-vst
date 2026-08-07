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
