"""Band routing: recognise a band-shaped set of controls, and order it.

Two INDEPENDENT catches stand between a set of controls and a band proposal, and
they rest on different evidence on purpose:

  1. the AXIS check  -- the varying token has to look like a band index, and a
     digit that also indexes something which is not part of a band is not a band
     index (§ device_axis_reason)
  2. the ORDER check -- every band needs a swept frequency, and the swept
     frequencies have to separate (§ order_bands)

Either one alone would look sufficient. A channel pair passes nothing: its
varying token is a channel marker AND its two members sweep the same range. That
redundancy is the safety, so test_bands.py asserts that DISABLING EITHER ONE lets
a trap through -- a catch that never fires is a catch nobody notices losing.

THE NAME GIVES THE GROUPING HYPOTHESIS. THE SWEEP GIVES THE ORDER. Two questions,
two sources. Ordinals are never taken from the digit in a name, from position in
the band vocabulary, or from parameter index.
"""
import math
import re

BAND_SEMANTICS = {"freq_hz", "gain_db", "q"}

# Semantics a DEVICE has one of, never a band. Used to tell a band index from an
# instance index: a band has no output level.
DEVICE_SEMANTICS = {
    "input_db", "output_db", "makeup_db", "ceiling_db", "threshold_db", "knee_db",
    "ratio", "attack_ms", "release_ms", "mix_pct", "wet_pct", "drive",
    "sensitivity", "delay_time_ms", "feedback_pct", "reverb_decay_s", "predelay_ms",
}

# The ordered band vocabulary, and the fold that reaches the spelled-out and
# compound conventions the corpus actually uses. Deliberately LOCAL to the
# proposer: widening prefixOrder() changes the hand path's inference, has its own
# gate and its own blast radius, and is queue item 0's decision, not this one's.
BAND_ORDER = ["LF", "LMF", "MF", "HMF", "HF"]
FOLD = {
    "LF": "LF", "LOW": "LF", "LO": "LF",
    "LMF": "LMF", "LOWMID": "LMF", "LOMID": "LMF", "LOWERMID": "LMF",
    "MF": "MF", "MID": "MF", "MIDDLE": "MF",
    "HMF": "HMF", "HIGHMID": "HMF", "HIMID": "HMF", "UPPERMID": "HMF",
    "HF": "HF", "HIGH": "HF", "HI": "HF",
}

CHANNEL_TOKENS = {"L", "R", "M", "S", "L/M", "R/S", "M/S", "MID/SIDE", "CH", "CHANNEL"}


def tokenise(name):
    """Split on whitespace, camelCase boundaries, and digit runs.

    A slash-joined pair stays ONE token: 'Freq L/M' is [Freq, L/M], because
    'L/M' names one channel, not two tokens that happen to be adjacent.
    """
    out = []
    for chunk in name.split():
        if "/" in chunk and not chunk.strip("/").isdigit():
            out.append(chunk.upper())
            continue
        for piece in re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z]+|[a-z]+|\d+", chunk):
            out.append(piece.upper())
    return out


def find_axis(names):
    """The one token position where these names differ, all others identical.

    Returns (position, [token per name]) or None. Kept for the simple case and
    for the gate; route() uses find_axis_subset.
    """
    got = find_axis_subset(names)
    if got is None or len(got[1]) != len(names):
        return None
    pos, mapping = got
    order = {n: t for t, n in mapping.items()}
    return pos, [order[n] for n in names]


def find_axis_subset(names):
    """The LARGEST subset of these names that differs in exactly one token.

    All-or-nothing was the first version and it is too brittle for real plugins:
    one unrelated control that happens to want the same semantic (AMEK's 'Mono
    Maker' among the band frequencies) killed the whole set. Leftovers are not
    an error -- they are unassigned, which is what the proposal document already
    carried a slot for.

    Returns (position, {token: name}) or None.
    """
    best = None
    toks = {n: tokenise(n) for n in names}
    for n in names:
        for pos in range(len(toks[n])):
            group = {}
            base = toks[n][:pos] + toks[n][pos + 1:]
            for m in names:
                tm = toks[m]
                if len(tm) != len(toks[n]):
                    continue
                if tm[:pos] + tm[pos + 1:] == base:
                    group.setdefault(tm[pos], m)
            if len(group) >= 2 and (best is None or len(group) > len(best[1])):
                best = (pos, group)
    return best


def classify_axis(tokens):
    """'prefix' | 'digit' | 'channel' | None.

    'channel' is a REASON, not a gate -- a channel marker is refused because it
    is not band vocabulary, which classify_axis reports as None-by-another-name.
    Naming it separately only makes the refusal message honest.
    """
    up = [t.upper() for t in tokens]
    if all(t in CHANNEL_TOKENS or re.fullmatch(r"CH\d*", t) for t in up):
        return "channel"
    if all(t in FOLD for t in up) and len({FOLD[t] for t in up}) == len(up):
        return "prefix"
    if all(t.isdigit() for t in up):
        return "digit"
    return None


def device_axis_reason(axis_tokens, member_names, all_controls, semantic_of):
    """Why this axis indexes a DEVICE rather than a band, or None.

    A digit is ambiguous -- it can index a band, a channel, or a whole instance --
    so it has to earn the reading. A prefix like LMF is unambiguous and never
    comes here.

    Matched on the token VALUE wherever it sits, NOT on an absolute position:
    'High Shelf Frequency 1' carries its digit at position 3 and 'Output Level 1'
    at position 2, and a position-keyed comparison silently skips the very
    control that gives the axis away. That was the first version of this and it
    let BAX through the axis catch.

    Two kinds of evidence, either sufficient:
      - the axis appears on a DEVICE semantic (BAX's 'Output Level 1' / '... 2';
        a band has no output level)
      - the axis appears on a control carrying no band vocabulary at all
        (AMEK's 'Bypass 1' / 'Bypass 2', which is the TMT channel)
    """
    wanted = {t.upper() for t in axis_tokens}
    for c in all_controls:
        if c["name"] in member_names:
            continue
        toks = tokenise(c["name"])
        hit = [t for t in toks if t in wanted]
        if not hit:
            continue
        rest = [t for t in toks if t not in wanted]
        sem = semantic_of.get(c["name"])
        if sem in DEVICE_SEMANTICS:
            return (f"the axis also indexes {c['name']!r}, whose semantic {sem} "
                    f"belongs to the device -- a band has no {sem}")
        if not any(t in FOLD for t in rest):
            return (f"the axis also indexes {c['name']!r}, which carries no band "
                    f"vocabulary -- it indexes the plugin, not a band")
    return None


def _sort_key(freq_control):
    """The GEOMETRIC centre of the swept frequency range, or None.

    Geometric because frequency is laid out logarithmically, so the centre of
    15..780 Hz is 108 Hz and not 397 Hz.

    ONLY WHEN THE DISPLAY DECLARED 'hz'. UAD 4K Channel Strip is why: its 'LF
    Freq' declares hz and sweeps 34.6..404, while its 'HF Freq' declares NOTHING
    and sweeps 4.3..18.4 -- which is kHz, but that is an INFERENCE, and an absent
    unit is a measurement rather than a gap to fill. Ordering on the raw numbers
    would rank HF below LF by three orders of magnitude; ordering on an assumed
    kHz would be guessing. So a band whose frequency carries no declared unit
    simply does not supply an ordinal.

    Ordering therefore needs TWO pieces of measured evidence: a swept range and
    a declared hz unit.
    """
    if (freq_control.get("unit") or "").lower() != "hz":
        return None
    anchors = freq_control.get("anchors") or []
    vals = [a[0] for a in anchors if isinstance(a, list) and a and a[0] > 0]
    if not vals:
        return None
    return math.sqrt(min(vals) * max(vals))


def order_bands(bands, controls_by_name, axis_kind):
    """Assign ordinals from SWEPT frequencies, or decline to assign them.

    The second catch, and it rests on different evidence from the first: the
    measurement, not the name.

    Returns (bands, hard_refusal_reason, ordering_note).

      - a band with no usable swept frequency cannot be ordered by measurement.
        It is moved to unassigned rather than killing the set -- real plugins
        have partially swept band sets (UAD 4K has four band gains and two swept
        band frequencies).
      - frequencies that DO NOT SEPARATE are refused outright on a DIGIT axis,
        where the axis carries no band evidence of its own and identical ranges
        are what a channel pair looks like from the measurement side.
      - on a PREFIX axis they are not. AMEK EQ 200's LF and LMF both sweep
        15..780 Hz and its HMF and HF both sweep 370..26000 Hz -- real console
        behaviour, not duplication, and LF vs LMF is unambiguous band vocabulary.
        The grouping stands and the ORDERING is left unresolved, because the one
        thing that must never happen is an ordinal taken from the name.
    """
    keyed, dropped = [], []
    for b in bands:
        freq_name = b["members"].get("freq_hz")
        key = _sort_key(controls_by_name[freq_name]) if freq_name else None
        if key is None:
            dropped.append((b, "no swept frequency with a declared hz unit, so no ordinal can be measured"))
        else:
            keyed.append((key, b, freq_name))

    if len(keyed) < 2:
        return None, (f"only {len(keyed)} band(s) carry a swept frequency with a declared "
                      f"hz unit, so an order cannot be derived from measurement"), None

    ties = [(a, c) for i, (a, _, _) in enumerate(keyed)
            for c, _, _ in keyed[i + 1:] if max(a, c) <= min(a, c) * 1.05]
    if ties:
        lo, hi = ties[0]
        if axis_kind == "digit":
            return None, (f"two bands sweep the same frequency range ({lo:.1f} Hz and "
                          f"{hi:.1f} Hz) on a digit axis -- they are not distinct bands, "
                          f"which is what a channel pair looks like"), None
        out = [{**b, "ordinal": None,
                "order_evidence": {"from": "unresolved",
                                   "why": f"ties with another band at {lo:.1f} Hz"}}
               for _, b, _ in keyed]
        return out + [{**b, "ordinal": None,
                       "order_evidence": {"from": "unresolved", "why": w}}
                      for b, w in dropped], None, \
               (f"ordering unresolved: two bands sweep the same range ({lo:.1f} Hz). "
                f"The grouping stands; ordinals are for the matcher to settle.")

    keyed.sort(key=lambda x: x[0])
    out = []
    for ordinal, (key, b, freq_name) in enumerate(keyed, 1):
        fc = controls_by_name[freq_name]
        vals = [a[0] for a in fc.get("anchors") or [] if isinstance(a, list) and a]
        out.append({**b, "ordinal": ordinal,
                    "order_evidence": {"from": "swept", "freq_index": fc["index"],
                                       "swept_hz": [min(vals), max(vals)],
                                       "sort_key": round(key, 3)}})
    note = None
    if dropped:
        note = (f"{len(dropped)} band(s) left unordered for want of a swept frequency")
    return out, None, note


def route(controls, semantic_of):
    """Recognise band-shaped sets among controls proposed a band semantic.

    controls     : list of the map's control dicts (name, index, anchors, ...)
    semantic_of  : {control name -> proposed semantic}

    Returns (proposal | None, reason_if_refused).

    Bands are assembled per SEMANTIC first -- 'LF Freq' and 'LMF Gain' differ in
    two positions, so an axis is only visible within the freq set, within the
    gain set, and so on -- then joined on the axis token they share. Token sets
    need NOT match across semantics: a real plugin often has four band gains and
    only two swept band frequencies, and demanding they agree refuses the set
    outright instead of grouping what is there.
    """
    by_name = {c["name"]: c for c in controls}
    members = [c for c in controls if semantic_of.get(c["name"]) in BAND_SEMANTICS]
    if len(members) < 4:
        return None, (f"only {len(members)} band-semantic controls: a band set needs at "
                      f"least two bands of at least two members")

    per_semantic, axis_kind, unassigned = {}, None, []
    for semantic in sorted(BAND_SEMANTICS):
        names = sorted(c["name"] for c in members if semantic_of[c["name"]] == semantic)
        if len(names) < 2:
            unassigned += [(n, "only one control with this semantic") for n in names]
            continue
        found = find_axis_subset(names)
        if found is None:
            unassigned += [(n, "shares no single-token axis with its peers") for n in names]
            continue
        pos, mapping = found
        unassigned += [(n, "outside the largest axis-sharing set")
                       for n in names if n not in mapping.values()]

        kind = classify_axis(list(mapping))
        if kind == "channel":
            return None, (f"the varying token {sorted(mapping)} is a channel marker, not a "
                          f"band index -- one band across two channels, not two bands")
        if kind is None:
            return None, (f"the varying token {sorted(mapping)} is neither band vocabulary "
                          f"nor a digit run -- no band axis")
        if kind == "digit":
            why = device_axis_reason(list(mapping), set(mapping.values()),
                                     controls, semantic_of)
            if why:
                return None, f"digit axis refused: {why}"
        if axis_kind is None:
            axis_kind = kind
        elif kind != axis_kind:
            return None, (f"the {semantic} controls group on a {kind} axis but another "
                          f"semantic groups on a {axis_kind} axis")
        per_semantic[semantic] = mapping

    if len(per_semantic) < 2:
        return None, "only one band semantic repeats: not enough to form a band"

    label_of = (lambda t: FOLD[t.upper()]) if axis_kind == "prefix" else (lambda t: t)
    bands, tokens = [], set().union(*(set(m) for m in per_semantic.values()))
    for token in tokens:
        got = {s: m[token] for s, m in per_semantic.items() if token in m}
        if len(got) < 2:
            unassigned += [(n, f"band {label_of(token)!r} has only this one member")
                           for n in got.values()]
            continue
        bands.append({"label": label_of(token), "axis_token": token, "members": got})

    if len(bands) < 2:
        return None, (f"only {len(bands)} band(s) have two or more members")

    ordered, why, note = order_bands(bands, by_name, axis_kind)
    if ordered is None:
        return None, why
    # A band that could not be ordered is only "left out" when OTHER bands were
    # ordered without it. When NOTHING could be ordered the whole set is simply
    # unordered -- listing every member as left out while also showing it in the
    # grouping is a contradiction the reviewer has to resolve for us.
    kept = [b for b in ordered if b.get("ordinal") is not None]
    if kept:
        for b in ordered:
            if b.get("ordinal") is None:
                unassigned.append(
                    (b["members"].get("freq_hz") or next(iter(b["members"].values())),
                     b["order_evidence"].get("why", "no measured order")))
        ordered = kept

    return {"grouping_source": "model-proposed", "axis": axis_kind,
            "ordering": "swept" if any(b["ordinal"] for b in ordered) else "unresolved",
            "ordering_note": note,
            "bands": [{"ordinal": b["ordinal"], "label": b["label"],
                       "order_evidence": b["order_evidence"],
                       "members": {s: {"index": by_name[n]["index"], "name": n}
                                   for s, n in b["members"].items()}}
                      for b in ordered],
            "unassigned": [{"name": n, "index": by_name[n]["index"], "why": w}
                           for n, w in unassigned]}, None
