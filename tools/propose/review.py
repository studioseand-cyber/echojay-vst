#!/usr/bin/env python3
"""Stage 5 -- review what stage 2 could not settle.

    python3 review.py                 # everything escalated, not yet decided
    python3 review.py --only CLA-76
    python3 review.py --summary       # counts only, decide nothing

READING IS NOT TOUCHING. Accepting a proposal records that a human LOOKED, which
is real and worth recording, and is NOT the same claim as having moved the
control. Only a correction or a verify-by-touch produces human-verified trust.
A list of confident claims invites blind acceptance, so acceptance is not allowed
to launder llm-classified into human-verified.

Rows are sorted by SEMANTIC, then category -- the throughput win is deciding
twenty input_db-vs-drive calls in a row, not context-switching per plugin. And
the evidence is printed above the claim, every time, because a claim read before
its evidence is a claim you have already half-accepted.

Decisions land in <root>/decisions/<fp>.json as they are made, so quitting
mid-pile costs nothing. Corrections ALSO append to misclassified-<run>.jsonl --
a correction is a dataset, not a burial.
"""
import argparse, collections, datetime, glob, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evidence import VOCAB, unit_family_conflict
from bands import route, tokenise, find_axis, classify_axis


def _map_controls(root, fp):
    for f in glob.glob(os.path.join(root, "maps", "*.json")):
        d = json.load(open(f))
        if d.get("fp") == fp:
            return list((d.get("controls") or {}).values())
    return []


def _band_set(root, doc):
    """The band grouping bands.py recognises for this plugin, or None.

    Collapsing these is the whole point: a five-band EQ escalates fifteen
    freq/gain/q rows that are ONE question -- 'is this grouping right?' -- and
    answering it fifteen times is the wrong shape of work.
    """
    controls = _map_controls(root, doc["fp"])
    if not controls:
        return None
    semantic_of = {p["control_name"]: p["kind"] for p in doc["params"]}
    for e in doc.get("escalations", []):
        sems = {a["semantic"] for a in e.get("arms", [])}
        if len(sems) == 1:
            semantic_of[e["name"]] = sems.pop()
    got, _ = route(controls, semantic_of)
    return got


# Outcomes that SETTLE a row. "deferred" is deliberately not among them: defer
# means "not now", and a deferred row that never returns is indistinguishable
# from a decided one. Found 5 Aug 2026 by deferring two partial band sets and
# watching them vanish from the pile.
TERMINAL = {"accepted", "corrected", "not_a_dial", "verify_by_touch",
            "band_grouping_confirmed", "band_grouping_rejected",
            "band_set_needs_manual_entry",
            "cohort_channels", "cohort_bands", "cohort_distinct"}


def cohorts(doc, map_params, taken):
    """Escalated rows on one plugin that claim the SAME semantic.

    THE COLLAPSE USED TO KEY ON A SUCCESSFUL GROUPING, so when route() refused --
    a channel axis, no single axis, too few members -- every row fell through
    individually with no memory between them. The mapper then answered fifteen
    fragments of one question, and an audit found 22 of 46 duplicate-semantic
    collisions were exactly that.

    The detector was already there: gate 4 fires on these cohorts. This keys on
    the cohort instead of on the grouping, so a set that cannot be GROUPED can
    still be ANSWERED once.
    """
    by_sem = collections.defaultdict(list)
    # SEED WITH WHAT THE MAP ALREADY HOLDS. PuigChild's 'Left Input' and
    # Vertigo's 'OutGnL' are already params, so only their opposite channel is
    # escalated -- and a cohort that only counts escalated rows sees one member
    # and skips it, while the collision is real and lands on the map. Including
    # the map's member makes the pair visible as a pair.
    for sem, prm in (map_params or {}).items():
        by_sem[sem].append({"name": prm.get("name"), "index": prm.get("index"),
                            "evidence": {"index": prm.get("index"),
                                         "range": None, "unit": prm.get("unit")},
                            "in_map": True})
    for e in doc.get("escalations", []):
        if e["name"] in taken:
            continue
        sems = {a["semantic"] for a in e.get("arms", [])}
        if len(sems) == 1:
            sem = sems.pop()
            if sem not in ("none", "__absent__"):
                by_sem[sem].append(e)

    out = []
    for sem, rows in by_sem.items():
        if len(rows) < 2 or not any(not r.get("in_map") for r in rows):
            continue    # nothing escalated here: the map alone is not a question
        names = [r["name"] for r in rows]
        ax = find_axis(sorted(names))
        kind = None
        if ax:
            pos, toks = ax
            first = tokenise(sorted(names)[0])
            kind = classify_axis(toks, first[:pos] + first[pos + 1:])
        out.append({"semantic": sem, "rows": rows, "axis_kind": kind,
                    "axis_tokens": (ax[1] if ax else None)})
    return out


def load_pile(root, only):
    maps_by_fp = {}
    for f in glob.glob(os.path.join(root, 'maps', '*.json')):
        d = json.load(open(f)); maps_by_fp[d['fp']] = d
    prop_dir, dec_dir = os.path.join(root, "proposals"), os.path.join(root, "decisions")
    if not os.path.isdir(prop_dir):
        return [], {}
    decided, pile = {}, []
    for fn in sorted(os.listdir(prop_dir)):
        if not fn.endswith(".json"):
            continue
        doc = json.load(open(os.path.join(prop_dir, fn)))
        if only and only.lower() not in (doc.get("plugin") or "").lower():
            continue
        dpath = os.path.join(dec_dir, fn)
        already = {}
        if os.path.exists(dpath):
            already = {d["index"]: d for d in json.load(open(dpath))["decisions"]
                       if d.get("outcome") in TERMINAL}
        decided[doc["fp"]] = already

        grouped = _band_set(root, doc)
        in_bands = set()
        if grouped:
            for b in grouped["bands"]:
                for m in b["members"].values():
                    in_bands.add(m["name"])

        rows = [e for e in doc.get("escalations", []) if e["index"] not in already]
        if grouped and any(e["name"] in in_bands for e in rows):
            key = "band_set:" + ",".join(
                str(m["index"]) for b in grouped["bands"] for m in b["members"].values())
            if key not in already:
                pile.append({"kind": "band_set", "index": key, "fp": doc["fp"],
                             "plugin": doc["plugin"], "category": doc.get("category"),
                             "grouping": grouped,
                             "covers": sorted(e["name"] for e in rows if e["name"] in in_bands)})
        # Cohorts over whatever the band collapse did not take.
        mp = (maps_by_fp.get(doc["fp"]) or {}).get("params") or {}
        for co in cohorts(doc, mp, in_bands):
            key = "cohort:" + co["semantic"] + ":" + ",".join(
                str(r["evidence"]["index"]) for r in
                sorted(co["rows"], key=lambda r: r["evidence"]["index"]))
            if key in already:
                for r in co["rows"]:
                    in_bands.add(r["name"])
                continue
            pile.append({"kind": "cohort", "index": key, "fp": doc["fp"],
                         "plugin": doc["plugin"], "category": doc.get("category"),
                         "cohort": co,
                         "covers": sorted(r["name"] for r in co["rows"])})
            for r in co["rows"]:
                in_bands.add(r["name"])

        for e in rows:
            if e["name"] in in_bands:
                continue                      # answered by a set question
            pile.append({**e, "kind": "control", "fp": doc["fp"], "plugin": doc["plugin"],
                         "category": doc.get("category")})
    return pile, decided


def sort_key(row):
    """By semantic, then category. Band sets first -- each one clears a dozen
    rows, so they are the cheapest questions in the pile per row retired."""
    if row["kind"] == "band_set":
        return (-2, "", row.get("category") or "", row["plugin"])
    if row["kind"] == "cohort":
        return (-1, row["cohort"]["semantic"], row.get("category") or "", row["plugin"])
    sems = sorted({a["semantic"] for a in row.get("arms", [])
                   if a["semantic"] not in ("none", "__absent__")})
    return (0 if sems else 1, sems[0] if sems else "", row.get("category") or "", row["plugin"])


HINT = {
    "channel": ("these look like ONE control across several channels",
                "keep one, the rest addressable by name"),
    "prefix":  ("these look like BANDS the grouping rules could not place",
                "manual band entry takes a typed frequency and still orders by sweep"),
    "digit":   ("these vary on a digit -- bands, channels or instances",
                "the digit alone does not say which"),
    None:      ("no single varying token: these may be genuinely different controls",
                "queue item 17 -- params holds one per semantic"),
}


def show_cohort(row, n, total):
    co = row["cohort"]
    what, hint = HINT[co["axis_kind"]]
    print("\n" + "=" * 74)
    print(f"{row['plugin']}   {row.get('category') or 'uncategorised'}"
          f"{'':>4}[{n} of {total}]")
    print()
    print(f"  {len(co['rows'])} CONTROLS ALL CLAIM {co['semantic']}  "
          f"-- one question, not {len(co['rows'])}")
    print(f"  {what}.")
    if co["axis_tokens"]:
        print(f"  they differ only in: {', '.join(co['axis_tokens'])}")
    print(f"  {hint}.")
    print()
    for r in sorted(co["rows"], key=lambda r: r["evidence"]["index"]):
        ev = r["evidence"]
        rng = "-" if not ev.get("range") else f"{ev['range'][0]:g}..{ev['range'][1]:g}"
        tag = "  (already in the map)" if r.get("in_map") else ""
        print(f"    [{ev['index']:3d}] {r['name']:24s} {rng:>14s}  "
              f"{ev['unit'] or ''}{tag}")
    print()
    print(f"  `params` is keyed by semantic, so only ONE of these can carry "
          f"{co['semantic']}.")


def decide_cohort(row):
    co = row["cohort"]
    keep = min(co["rows"], key=lambda r: (not r.get("in_map"), r["evidence"]["index"]))
    print()
    print(f"    [c] channels of one control -- {co['semantic']} goes to "
          f"[{keep['evidence']['index']}] {keep['name']!r}, rest by name")
    print(f"    [m] bands -- send the set to manual band entry")
    print(f"    [x] genuinely different controls (queue 17) -- defer the set")
    print(f"    [i] none of these, answer them one at a time")
    print(f"    [d] defer            [q] save and quit")
    while True:
        try:
            k = input("  > ").strip().lower()
        except EOFError:
            return None
        if k == "q":
            return None
        if k == "d":
            return {"outcome": "deferred"}
        if k == "i":
            return {"outcome": "cohort_answer_individually", "kind": "cohort",
                    "semantic": None, "covers": row["covers"]}
        if k in ("c", "m", "x"):
            oc = {"c": "cohort_channels", "m": "cohort_bands",
                  "x": "cohort_distinct"}[k]
            out = {"outcome": oc, "kind": "cohort", "claimed_semantic": co["semantic"],
                   "axis_kind": co["axis_kind"], "axis_tokens": co["axis_tokens"],
                   "members": [{"index": r["evidence"]["index"], "name": r["name"],
                                "in_map": bool(r.get("in_map"))}
                               for r in sorted(co["rows"],
                                               key=lambda r: r["evidence"]["index"])],
                   "covers": row["covers"],
                   "semantic_source": "human-corrected", "trust": "llm-classified"}
            if k == "c":
                # ONE carries the semantic; the rest stay name-addressed. The
                # lowest index is the default because L/first-channel sorts
                # first in every shape the corpus uses -- and it is SHOWN before
                # it is taken, so a wrong guess is visible rather than silent.
                out["semantic"] = co["semantic"]
                out["carried_by"] = {"index": keep["evidence"]["index"],
                                     "name": keep["name"]}
                out["name_addressed"] = [m for m in out["members"]
                                         if m["index"] != keep["evidence"]["index"]]
            return out
        print("  ?")


def show_band_set(row, n, total):
    """One question standing in for a dozen rows, so it has to show enough to
    answer honestly: every member, and where the ordering came from."""
    g = row["grouping"]
    print("\n" + "=" * 74)
    print(f"{row['plugin']}   {row.get('category') or 'uncategorised'}"
          f"{'':>4}[{n} of {total}]")
    print()
    if g.get("complete", True):
        print(f"  A BAND SET -- {len(g['bands'])} bands on a {g['axis']} axis, "
              f"standing in for {len(row['covers'])} rows")
    else:
        print(f"  A PARTIAL BAND SET -- the evidence offers "
              f"{g['bands_offered']} bands on a {g['axis']} axis and only "
              f"{len(g['bands'])} could be placed.")
        print(f"  MISSING: {', '.join(g['missing_bands'])}")
        print(f"  Proposing {len(g['bands'])} would claim this plugin HAS "
              f"{len(g['bands'])} bands, which is wrong about the device.")
    print()
    for b in g["bands"]:
        ordinal = f"#{b['ordinal']}" if b.get("ordinal") else " ?"
        members = "  ".join(f"{sem}=[{m['index']}] {m['name']!r}"
                            for sem, m in sorted(b["members"].items()))
        print(f"    {ordinal} {b['label']:<5} {members}")
    print()
    ev = g["bands"][0].get("order_evidence") or {}
    if g["ordering"] == "swept":
        print(f"  ordering: from the SWEPT frequencies "
              f"({', '.join(str(round(b['order_evidence']['sort_key'])) + ' Hz' for b in g['bands'])})")
    else:
        print(f"  ordering: UNRESOLVED -- {g.get('ordering_note') or ev.get('why', '')}")
        print(f"            the grouping is the question here; ordinals are the matcher's")
    left = g.get("unassigned") or []
    print()
    print(f"  band-semantic controls found: {len(row['covers']) + len(left)}   "
          f"placed in the grouping: {sum(len(b['members']) for b in g['bands'])}   "
          f"left out: {len(left)}")
    if left:
        for u in left:
            print(f"    [{u['index']}] {u['name']!r} -- {u['why']}")


def decide_band_set(row):
    g = row["grouping"]
    partial = not g.get("complete", True)
    print()
    if partial:
        # There is no subset to accept. The bands that could not be placed are
        # unmeasured, not absent, and manual band entry is the tool built for
        # exactly that -- it takes a typed frequency and still orders by sweep.
        print("    [m] send to manual band entry   [n] not a band set at all")
        print("    [d] defer                       [q] save and quit")
    else:
        print("    [y] the grouping is right       [n] it is not -- send the rows back")
        print("    [d] defer                       [q] save and quit")
    while True:
        try:
            k = input("  > ").strip().lower()
        except EOFError:
            return None
        if k == "q":
            return None
        if k == "d":
            return {"outcome": "deferred"}
        if partial and k == "m":
            return {"outcome": "band_set_needs_manual_entry", "kind": "band_set",
                    "axis": g["axis"], "ordering": g["ordering"],
                    "bands_offered": g["bands_offered"],
                    "missing_bands": g["missing_bands"],
                    "bands": [{"label": b["label"], "ordinal": b.get("ordinal"),
                               "members": b["members"]} for b in g["bands"]],
                    "covers": row["covers"],
                    "semantic_source": "human-corrected", "trust": "llm-classified"}
        if partial and k == "y":
            print("  There is no complete grouping to accept -- "
                  f"{', '.join(g['missing_bands'])} could not be placed.")
            continue
        if not partial and k in ("y", "n"):
            # Confirming a grouping is READING, exactly like waving through a
            # flat proposal: it records that a human looked. It writes no map --
            # the grouping still has to reach EjmapBands and survive the stride
            # against real captures before it becomes a group.
            return {"outcome": "band_grouping_confirmed" if k == "y" else "band_grouping_rejected",
                    "kind": "band_set", "axis": g["axis"], "ordering": g["ordering"],
                    "bands": [{"label": b["label"], "ordinal": b.get("ordinal"),
                               "members": b["members"]} for b in g["bands"]],
                    "covers": row["covers"],
                    "semantic_source": "human-confirmed" if k == "y" else "human-corrected",
                    "trust": "llm-classified"}
        print("  ?")


def show(row, n, total):
    ev = row["evidence"]
    rng = "null" if not ev.get("range") else f"{ev['range'][0]:g}..{ev['range'][1]:g}"
    spn = "null" if not ev.get("span") else f"{ev['span'][0]:g}..{ev['span'][1]:g}"
    print("\n" + "=" * 74)
    print(f"{row['plugin']}   {row.get('category') or 'uncategorised'}"
          f"{'':>4}[{n} of {total}]")
    print()
    print(f"  {row['name']!r}   index {ev['index']}")
    print(f"    kind {ev['kind']}   range {rng}   span {spn}   "
          f"unit {ev['unit'] or '(none declared)'}   anchors {ev['anchors']}")
    print()
    for a in row.get("arms", []):
        sem = a["semantic"] if a["semantic"] != "__absent__" else "(no answer)"
        print(f"    {a['model']:<18} {sem:<18} {a['confidence']}")
    if not row.get("arms"):
        print("    (no arm answered)")
    print()
    print(f"  ESCALATED: {row['why']}")


def menu(row):
    """The options offered are exactly the semantics an arm actually proposed,
    plus the vocabulary behind a second keystroke. Offering all 23 up front
    would make the common case slower to serve the rare one."""
    offered = []
    for a in row.get("arms", []):
        if a["semantic"] not in ("none", "__absent__") and a["semantic"] not in offered:
            offered.append(a["semantic"])
    return offered


def claims_for(root, fp, decided_here):
    """Every semantic already claimed on this plugin, and by what.

    THE SAME RULE THE WIZARD HAS. duplicateSemanticConflicts refuses one semantic
    on two indices at the review screen and the submit path, because `params` is
    keyed by semantic and the second write silently wins. The review tool was the
    one path that did not run it: 143 choices across 40 plugins with nothing
    watching, and an audit found 46 collisions of which 13 would have overwritten
    an existing param.
    """
    out = {}
    for f in glob.glob(os.path.join(root, "maps", "*.json")):
        d = json.load(open(f))
        if d.get("fp") != fp:
            continue
        for sem, prm in (d.get("params") or {}).items():
            out[sem] = (prm.get("index"), prm.get("name"), "the map")
        break
    for f in glob.glob(os.path.join(root, "decisions", "*.json")):
        d = json.load(open(f))
        if d.get("fp") != fp:
            continue
        for x in d["decisions"]:
            if x.get("outcome") in ("accepted", "corrected") and x.get("semantic"):
                out[x["semantic"]] = (x.get("index"), x.get("control_name"), "an earlier answer")
        break
    for sem, v in decided_here.items():
        out[sem] = v
    return out


def resolve_typed(text):
    """A typed semantic: exact, or an unambiguous prefix. None if neither.

    Typing beats counting to 23. It also means the full vocabulary is reachable
    without discovering [v] first -- which mattered, because the card prints no
    numbered options exactly when no model proposed anything, and that is
    precisely when the reviewer is the one who knows the answer.
    """
    t = text.strip().lower()
    if t in VOCAB:
        return t
    hits = [v for v in VOCAB if v.startswith(t)]
    return hits[0] if len(hits) == 1 else None


def decide(row, claimed=None):
    claimed = claimed or {}
    offered = menu(row)
    print()
    for i, s in enumerate(offered, 1):
        print(f"    [{i}] {s}")
    if not offered:
        print("    neither model proposed a semantic for this control.")
        print("    You can still name one -- type it (e.g. 'output_db', or 'out'),")
        print("    or [v] to list the vocabulary.")
    print("    [n] none / not a dial      [t] verify by touch (defer to the wizard)")
    print("    [v] the full vocabulary    [d] defer      [q] save and quit")
    print("    or type a semantic name directly")
    while True:
        try:
            k = input("  > ").strip().lower()
        except EOFError:
            return None
        if k == "q":
            return None
        if k == "d":
            return {"outcome": "deferred"}
        if k == "t":
            return {"outcome": "verify_by_touch"}
        if k == "n":
            # A "none" IS A DATASET, so capture what it should have been while
            # the reviewer is looking at it. Free text on purpose: the whole
            # point is that the right answer is not in the vocabulary yet, so a
            # menu cannot ask the question.
            try:
                wanted = input("    what would you have called it? "
                               "(enter to skip) > ").strip()
            except EOFError:
                wanted = ""
            out = {"outcome": "not_a_dial", "semantic": None,
                   "semantic_source": "human-corrected", "trust": "human-verified"}
            if wanted:
                out["wanted_semantic"] = wanted
            return out
        if k == "v":
            for i, s in enumerate(VOCAB, 1):
                print(f"    [{i:2d}] {s}")
            offered = VOCAB
            continue

        chosen = None
        if k.isdigit() and 1 <= int(k) <= len(offered):
            chosen = offered[int(k) - 1]
        elif k:
            chosen = resolve_typed(k)
            if chosen is None and not k.isdigit():
                near = [v for v in VOCAB if v.startswith(k)]
                if near:
                    print(f"  ambiguous -- did you mean: {', '.join(near)}")
                else:
                    print(f"  {k!r} is not in the vocabulary. [v] lists it; [n] records "
                          f"'none' and asks what you would have called it.")
                continue
        if chosen is None:
            print("  ?")
            continue

        conflict = unit_family_conflict(chosen, row["evidence"]["unit"])
        if conflict:
            # The gate refuses a human the same way it refuses a model.
            print(f"  REFUSED -- {conflict}")
            print("  Pick another, or [t] to verify it by touch first.")
            continue
        held = claimed.get(chosen)
        if held and held[0] != row["evidence"]["index"]:
            idx, nm, src = held
            print(f"  REFUSED -- {chosen} is already claimed on this plugin by "
                  f"[{idx}] {nm!r} ({src}).")
            print(f"  `params` is keyed by semantic, so writing it twice loses one "
                  f"of them silently.")
            print(f"  If these are bands or channels of one control, [d] defer the "
                  f"set and send it to manual entry.")
            continue
        proposed = {a["semantic"] for a in row.get("arms", [])}
        waved = len(proposed) == 1 and chosen in proposed
        return {"outcome": "accepted" if waved else "corrected",
                "semantic": chosen,
                # reading is not touching
                "semantic_source": "human-confirmed" if waved else "human-corrected",
                "trust": "llm-classified" if waved else "human-verified"}


def audit_duplicates(root, only):
    """The gate, run over decisions ALREADY made. Repeatable, so a pile worked
    before the gate existed can still be checked."""
    maps = {}
    for f in glob.glob(os.path.join(root, "maps", "*.json")):
        d = json.load(open(f)); maps[d["fp"]] = d
    total = collisions = overwrites = 0
    for f in sorted(glob.glob(os.path.join(root, "decisions", "*.json"))):
        doc = json.load(open(f))
        if only and only.lower() not in doc["plugin"].lower():
            continue
        m = maps.get(doc["fp"], {})
        existing = {sem: (p.get("index"), p.get("name"))
                    for sem, p in (m.get("params") or {}).items()}
        picked = collections.defaultdict(list)
        for x in doc["decisions"]:
            if x.get("outcome") in ("accepted", "corrected") and x.get("semantic"):
                total += 1
                picked[x["semantic"]].append((x.get("index"), x.get("control_name")))
        for sem, rows in sorted(picked.items()):
            hits_map = sem in existing and existing[sem][0] not in [i for i, _ in rows]
            if len(rows) > 1 or hits_map:
                collisions += 1
                overwrites += 1 if hits_map else 0
                print(f"\n  {doc['plugin'][:36]:36s} [{m.get('category')}] {sem}")
                for i, n in rows:
                    print(f"      decision  [{i}] {n!r}")
                if hits_map:
                    print(f"      MAP       [{existing[sem][0]}] {existing[sem][1]!r} "
                          f"<- this would be overwritten")
    print(f"\n{total} semantic choices, {collisions} collision(s), "
          f"{overwrites} of which would overwrite an existing map param")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--only", default=None)
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--audit-duplicates", action="store_true",
                    help="report semantics claimed twice on one plugin, and stop")
    args = ap.parse_args()

    if args.audit_duplicates:
        audit_duplicates(args.root, args.only)
        return

    pile, decided = load_pile(args.root, args.only)
    if not pile:
        print("nothing to review")
        return
    pile.sort(key=sort_key)

    if args.summary:
        def label(r):
            if r["kind"] == "band_set":
                return "BAND SET (stands in for many rows)"
            if r["kind"] == "cohort":
                k = r["cohort"]["axis_kind"] or "no shared axis"
                return f"COHORT, {k} (stands in for {len(r['covers'])} rows)"
            return (r.get("why") or "?").split(":")[0]
        why = collections.Counter(label(r) for r in pile)
        covered = sum(len(r["covers"]) for r in pile
                      if r["kind"] in ("band_set", "cohort"))
        sets = sum(1 for r in pile if r["kind"] in ("band_set", "cohort"))
        print(f"{len(pile)} questions across {len({r['plugin'] for r in pile})} plugins")
        if sets:
            print(f"  ({sets} of them are band sets, standing in for {covered} rows -- "
                  f"{covered - sets} fewer questions)")
        print()
        for w, n in why.most_common():
            print(f"  {n:4d}  {w}")
        return

    dec_dir = os.path.join(args.root, "decisions")
    os.makedirs(dec_dir, exist_ok=True)
    run_id = datetime.datetime.now().strftime("%Y%m%dT%H%M%S")
    mis_path = os.path.join(args.root, f"misclassified-{run_id}.jsonl")

    print(f"{len(pile)} rows to review. [q] saves and quits at any point.")
    made = 0
    claimed = collections.defaultdict(dict)
    for n, row in enumerate(pile, 1):
        if row["kind"] == "cohort":
            show_cohort(row, n, len(pile))
            d = decide_cohort(row)
        elif row["kind"] == "band_set":
            show_band_set(row, n, len(pile))
            d = decide_band_set(row)
        else:
            show(row, n, len(pile))
            d = decide(row, claims_for(args.root, row['fp'], claimed[row['fp']]))
        if d is None:
            break
        record = {"index": row["index"],
                  "control_name": row.get("name"),
                  "why_escalated": row.get("why"), "arms": row.get("arms", []),
                  "at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                  **d}

        path = os.path.join(dec_dir, f"{row['fp']}.json")
        doc = json.load(open(path)) if os.path.exists(path) else \
            {"fp": row["fp"], "plugin": row["plugin"], "decisions": []}
        doc["decisions"] = [x for x in doc["decisions"] if x["index"] != row["index"]]
        doc["decisions"].append(record)
        json.dump(doc, open(path, "w"), indent=1)
        made += 1

        # A correction is a dataset, not a burial (EjmapAssignment.h).
        if d.get("semantic"):
            # A cohort answer claims the semantic for ONE named member; a plain
            # row claims it for itself.
            holder = d.get("carried_by") or {"index": (row.get("evidence") or {}).get("index"),
                                             "name": row.get("name")}
            claimed[row["fp"]][d["semantic"]] = (holder.get("index"),
                                                 holder.get("name"), "this session")
        if d["outcome"] == "corrected":
            with open(mis_path, "a") as f:
                f.write(json.dumps({"fp": row["fp"], "plugin": row["plugin"],
                                    "control": row["name"], "chose": d.get("semantic"),
                                    "arms": row.get("arms", []),
                                    "why_escalated": row["why"]}) + "\n")

    deferred = sum(1 for r in pile[:made] if False)  # placeholder, see below
    print(f"\n{made} decided, {len(pile)-made} left.  -> {dec_dir}")
    print("   (deferred rows return next run -- defer means not now, not never)")
    if os.path.exists(mis_path):
        print(f"corrections recorded -> {mis_path}")


if __name__ == "__main__":
    main()
