#!/usr/bin/env python3
"""Re-derive accept/escalate from proposals ALREADY ON DISK, without re-paying.

    python3 regate.py            # dry run: what would move
    python3 regate.py --apply

WHY THIS IS POSSIBLE AT ALL. Every proposal row records BOTH ARMS -- model,
semantic, confidence -- alongside the evidence the gates read. A gate is a pure
function of those, so changing it is a re-derivation rather than a re-run. The
7 Aug hedged-agreement change moved ~36% of agreements into the accept set and
cost nothing to apply to work already paid for.

WHAT IT WILL NOT DO. It never invents an answer the arms did not give, and it
never touches a row a HUMAN has decided -- a decision file entry outranks any
gate, because the gate is a machine and the human looked.
"""
import argparse, collections, datetime, glob, json, os, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evidence import unit_family_conflict


def regate_one(doc):
    """Apply the CURRENT gate to rows already on disk. Returns (doc, moved)."""
    rows = []
    for r in (doc.get("params") or []):
        rows.append(("accepted", r))
    for r in (doc.get("escalations") or []):
        rows.append(("escalated", r))

    accepted, escalated, moved = [], [], 0
    for was, r in rows:
        arms = {a.get("model"): a for a in (r.get("arms") or [])}
        sems = [a.get("semantic") for a in arms.values()]
        confs = [a.get("confidence") for a in arms.values()]
        why = r.get("why") or ""

        # Only the confidence gate changed. Anything escalated for another
        # reason stays escalated: re-deriving those would be a different
        # change wearing this one's clothes.
        if was == "escalated" and "declined to be confident" not in why:
            escalated.append(r)
            continue

        if len(sems) != 2 or sems[0] != sems[1] or sems[0] in ("none", "__absent__"):
            escalated.append(r)
            continue

        ev = r.get("evidence") or {}
        conflict = unit_family_conflict(sems[0], ev.get("unit"))
        if conflict:
            escalated.append({**r, "why": f"unit family: {conflict}"})
            continue

        hedged = not all(c == "high" for c in confs)
        row = {k: v for k, v in r.items() if k != "why"}
        row["kind"] = sems[0]
        row["semantic"] = sems[0]
        row["hedged"] = hedged
        row["reason"] = ("both arms agree" + ("; one hedged" if hedged else " and are confident")
                         + " -- re-derived by regate.py, gate of 7 Aug 2026")
        accepted.append(row)
        if was == "escalated":
            moved += 1

    # gate 4 unchanged: one semantic may not be claimed by two indices
    seen = collections.defaultdict(list)
    for r in accepted:
        seen[r["semantic"]].append(r)
    keep = []
    for semantic, rs in seen.items():
        if len(rs) == 1:
            keep.append(rs[0])
        else:
            names = ", ".join(repr(r.get("name")) for r in rs)
            for r in rs:
                escalated.append({**{k: v for k, v in r.items() if k != "semantic"},
                                  "why": f"{len(rs)} controls claim {semantic}: {names}"})
                moved -= 1
    doc["params"] = keep
    doc["escalations"] = escalated
    return doc, moved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    files = [f for f in glob.glob(os.path.join(args.root, "proposals", "*.json"))
             if not os.path.isdir(f)]
    before_a = before_e = after_a = after_e = 0
    total_moved = 0
    docs = []
    for f in files:
        try:
            d = json.load(open(f))
        except Exception:
            continue
        before_a += len(d.get("params") or [])
        before_e += len(d.get("escalations") or [])
        d, moved = regate_one(d)
        after_a += len(d.get("params") or [])
        after_e += len(d.get("escalations") or [])
        total_moved += moved
        docs.append((f, d))

    print(f"{len(docs)} proposal file(s)")
    print(f"  accepted   {before_a:7,}  ->  {after_a:7,}   ({after_a-before_a:+,})")
    print(f"  escalated  {before_e:7,}  ->  {after_e:7,}   ({after_e-before_e:+,})")
    hedged = sum(1 for _f, d in docs for r in (d.get("params") or []) if r.get("hedged"))
    print(f"  of the new accept set, {hedged:,} are hedged agreements")

    if not args.apply:
        print("\nDry run. Nothing written. Add --apply.")
        return 0

    # TIMESTAMPED, AND ALWAYS TAKEN. This used to be a fixed name guarded by
    # `if not os.path.exists(backup)`, which meant the second --apply silently
    # kept the FIRST run's snapshot: on 8 Aug that snapshot held 183 files while
    # the corpus held 1,106, and it printed nothing to say so. A backup whose
    # presence is checked instead of its contents is not a backup, it is a
    # directory with a reassuring name.
    backup = os.path.join(args.root, "proposals-backup-regate-"
                          + datetime.datetime.now().strftime("%Y%m%dT%H%M%S"))
    # copytree FOLLOWS a symlinked source (verified: distinct inodes out), which
    # matters because ~/Library/ejmap/proposals is one. Shell `cp -R` does not,
    # and produced a second symlink to the live corpus on 8 Aug.
    shutil.copytree(os.path.join(args.root, "proposals"), backup, symlinks=True)
    n = len([f for f in os.listdir(backup) if f.endswith(".json")])
    print(f"backed up {n} file(s) -> {backup}")
    for f, d in docs:
        json.dump(d, open(f, "w"), indent=1)
    print(f"rewrote {len(docs)} proposal file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
