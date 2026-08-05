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
import argparse, datetime, glob, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evidence import VOCAB, unit_family_conflict
from bands import route


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
            "band_set_needs_manual_entry"}


def load_pile(root, only):
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
        for e in rows:
            if e["name"] in in_bands:
                continue                      # answered by the band-set question
            pile.append({**e, "kind": "control", "fp": doc["fp"], "plugin": doc["plugin"],
                         "category": doc.get("category")})
    return pile, decided


def sort_key(row):
    """By semantic, then category. Band sets first -- each one clears a dozen
    rows, so they are the cheapest questions in the pile per row retired."""
    if row["kind"] == "band_set":
        return (-1, "", row.get("category") or "", row["plugin"])
    sems = sorted({a["semantic"] for a in row.get("arms", [])
                   if a["semantic"] not in ("none", "__absent__")})
    return (0 if sems else 1, sems[0] if sems else "", row.get("category") or "", row["plugin"])


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


def decide(row):
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
        proposed = {a["semantic"] for a in row.get("arms", [])}
        waved = len(proposed) == 1 and chosen in proposed
        return {"outcome": "accepted" if waved else "corrected",
                "semantic": chosen,
                # reading is not touching
                "semantic_source": "human-confirmed" if waved else "human-corrected",
                "trust": "llm-classified" if waved else "human-verified"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--only", default=None)
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args()

    pile, decided = load_pile(args.root, args.only)
    if not pile:
        print("nothing to review")
        return
    pile.sort(key=sort_key)

    if args.summary:
        import collections
        why = collections.Counter(
            "BAND SET (stands in for many rows)" if r["kind"] == "band_set"
            else r["why"].split(":")[0] for r in pile)
        covered = sum(len(r["covers"]) for r in pile if r["kind"] == "band_set")
        sets = sum(1 for r in pile if r["kind"] == "band_set")
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
    for n, row in enumerate(pile, 1):
        if row["kind"] == "band_set":
            show_band_set(row, n, len(pile))
            d = decide_band_set(row)
        else:
            show(row, n, len(pile))
            d = decide(row)
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
