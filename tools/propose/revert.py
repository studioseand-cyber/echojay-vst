#!/usr/bin/env python3
"""Withdraw review decisions in a batch, by category and control name.

    python3 revert.py --category amp_sim --names Presence,Bass,Middle,Treble,Tr,Midle
    python3 revert.py --category amp_sim --names ... --semantic gain_db --apply

Dry run by default. Nothing moves without --apply.

A WITHDRAWN DECISION IS NOT A DELETED ONE. The entry stays, its outcome becomes
"reverted" -- which is not terminal, so the row returns to the pile -- and the
original is preserved under `superseded`. That a semantic was chosen and later
withdrawn is evidence about the VOCABULARY, not a mistake to erase: the amp-sim
tone controls were withdrawn because the dial set has no home for them, and the
record of that is what makes the case.

Names match on TOKENS, not substrings. 'Ch64Bass' tokenises to [CH,64,BASS] and
matches BASS; 'Bassoon' would not. Abbreviations you pass ('Tr' for Treble) are
matched the same way and reported separately so a guess never hides in a batch.
"""
import argparse, datetime, glob, json, os, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bands import tokenise


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--category", default=None)
    ap.add_argument("--names", default="", help="comma-separated name tokens")
    ap.add_argument("--semantic", default=None, help="only withdraw this chosen semantic")
    ap.add_argument("--reason", default="withdrawn in a batch")
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    wanted = {n.strip().upper() for n in args.names.split(",") if n.strip()}
    cats = {}
    for f in glob.glob(os.path.join(args.root, "maps", "*.json")):
        d = json.load(open(f))
        cats[d["fp"]] = (d["identity"]["name"], d.get("category"))

    hits, skipped = [], []
    for f in sorted(glob.glob(os.path.join(args.root, "decisions", "*.json"))):
        doc = json.load(open(f))
        name, cat = cats.get(doc["fp"], (doc["plugin"], None))
        for x in doc["decisions"]:
            if x.get("outcome") == "reverted":
                continue
            ctrl = x.get("control_name")
            if not ctrl:
                continue
            match_cat = args.category is None or cat == args.category
            toks = set(tokenise(ctrl))
            match_name = not wanted or bool(toks & wanted)
            match_sem = args.semantic is None or x.get("semantic") == args.semantic
            row = (f, name, cat, ctrl, x.get("outcome"), x.get("semantic"), sorted(toks & wanted))
            if match_cat and match_name and match_sem:
                hits.append(row)
            elif match_cat and match_sem and wanted:
                skipped.append(row)

    print(f"MATCHED {len(hits)} decision(s)"
          + (f" in category {args.category}" if args.category else "")
          + (f" whose chosen semantic is {args.semantic}" if args.semantic else ""))
    exact = [h for h in hits if any(t == h[3].upper() for t in h[6])]
    partial = [h for h in hits if h not in exact]
    for label, rows in (("matched on the whole control name", exact),
                        ("matched on a TOKEN inside the name -- check these", partial)):
        if not rows:
            continue
        print(f"\n  {label}:")
        for _, name, cat, ctrl, outcome, sem, why in rows:
            print(f"    {name[:32]:32s} {ctrl:22s} {outcome:10s} {sem}   <- {','.join(why)}")

    if skipped:
        print(f"\n  NOT matched, same category and semantic ({len(skipped)}) -- left alone:")
        for _, name, cat, ctrl, outcome, sem, _w in skipped:
            print(f"    {name[:32]:32s} {ctrl:22s} {outcome:10s} {sem}")

    if not args.apply:
        print("\nDry run. Nothing changed. Add --apply to withdraw these.")
        return
    if not hits:
        return

    backup = os.path.join(args.root, "decisions-backup-"
                          + datetime.datetime.now().strftime("%Y%m%dT%H%M%S"))
    shutil.copytree(os.path.join(args.root, "decisions"), backup)
    print(f"\nbacked up -> {backup}")

    at = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    targets = {}
    for f, _n, _c, ctrl, _o, _s, _w in hits:
        targets.setdefault(f, set()).add(ctrl)
    n = 0
    for f, ctrls in targets.items():
        doc = json.load(open(f))
        for x in doc["decisions"]:
            if x.get("control_name") in ctrls and x.get("outcome") != "reverted":
                x["superseded"] = {k: x.get(k) for k in
                                   ("outcome", "semantic", "semantic_source", "trust")}
                x["outcome"] = "reverted"
                x["reverted_at"] = at
                x["reverted_reason"] = args.reason
                x.pop("semantic", None)
                x.pop("trust", None)
                x.pop("semantic_source", None)
                n += 1
        json.dump(doc, open(f, "w"), indent=1)
    print(f"withdrew {n} decision(s); those rows return to the pile on the next run")


if __name__ == "__main__":
    main()
