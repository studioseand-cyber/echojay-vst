#!/usr/bin/env python3
"""What stage 2 produced, at whatever scale it has run to.

    python3 pipeline_report.py                    # everything proposed
    python3 pipeline_report.py --maps-from f.json # just that slice

THREE QUESTIONS, and they are different:

  1. THE RATES -- accept, escalate, decline -- against the population that
     actually produced them. The pilot's 6.4% accept came from 40 maps that
     were 11 compressors and one reverb; a rate from a different population is
     a different number, not a better or worse one.

  2. THE REVIEW PILE, with the auto-decline already applied. An escalation
     rate is not a workload: a confident agreed `none` is a DECLINE, measured
     57/57 in the pilot, and counting it as review makes the pile look
     unworkable when it is not. The number that matters is what a human would
     actually be asked.

  3. THE NONE CLUSTERS. Controls both arms call `none` are not failures --
     they are the vocabulary telling you what it has no word for. Grouped by
     token across plugins, a cluster on many plugins is a semantic; a cluster
     on one is a quirk. This is the question the 40-map corpus could not
     answer.
"""
import argparse, collections, glob, json, os, re, sys


def load(root, only_paths=None):
    fps = None
    if only_paths:
        fps = {json.load(open(p))["fp"] for p in json.load(open(os.path.expanduser(only_paths)))}
    out = []
    for f in glob.glob(os.path.join(root, "proposals", "*.json")):
        if os.path.isdir(f):
            continue
        try:
            d = json.load(open(f))
        except Exception:
            continue
        if fps is None or d.get("fp") in fps:
            out.append(d)
    return out


STOP = {"ch", "l", "r", "m", "s", "in", "out", "on", "off", "the", "a", "b",
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "and", "to"}


def tokens(name):
    parts = re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z]+|[a-z]+|\d+", name or "")
    return [p.lower() for p in parts if p.lower() not in STOP and not p.isdigit()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--maps-from", default=None)
    ap.add_argument("--clusters", type=int, default=30)
    args = ap.parse_args()

    props = load(args.root, args.maps_from)
    if not props:
        print("no proposals found")
        return 1

    acc = sum(len(p.get("params") or {}) for p in props)
    esc = sum(len(p.get("escalations") or []) for p in props)
    dec = sum(len(p.get("declines") or []) for p in props)
    tot = acc + esc + dec

    print("=" * 74)
    print(f"1. RATES over {len(props)} proposed map(s), {tot:,} controls asked about")
    print("=" * 74)
    for label, n in (("accepted", acc), ("escalated", esc), ("declined", dec)):
        print(f"  {n:7,}  {100*n/max(1,tot):5.1f}%  {label}")

    # ---- 2. the pile a human is actually asked -----------------------------
    print("\n" + "=" * 74)
    print("2. THE REVIEW PILE, auto-decline applied")
    print("=" * 74)
    by_reason = collections.Counter()
    agreed_none, hedged_none = 0, 0
    real_pile = []
    for p in props:
        for e in (p.get("escalations") or []):
            why = (e.get("why") or e.get("why_escalated") or "?").split(":")[0]
            arms = {a.get("model"): a for a in (e.get("arms") or [])}
            sems = [a.get("semantic") for a in arms.values()]
            confs = [a.get("confidence") for a in arms.values()]
            if sems and all(s == "none" for s in sems):
                # BOTH SAY NONE. Confident -> decline outright (57/57 measured).
                # Hedged -> still a decline for workload purposes, but counted
                # apart so the claim stays honest about which is which.
                if all(c == "high" for c in confs):
                    agreed_none += 1
                else:
                    hedged_none += 1
                continue
            by_reason[why] += 1
            real_pile.append((p, e))

    print(f"  escalations as recorded            {esc:7,}")
    print(f"  minus both-arms-none, confident   -{agreed_none:7,}   auto-declined (57/57 in the pilot)")
    print(f"  minus both-arms-none, hedged      -{hedged_none:7,}   still nobody's dial")
    print(f"  = WHAT A HUMAN IS ASKED            {len(real_pile):7,}   "
          f"{100*len(real_pile)/max(1,tot):.1f}% of controls")
    print()
    print("  by escalation reason:")
    for why, n in by_reason.most_common():
        print(f"    {n:6,}  {100*n/max(1,len(real_pile)):5.1f}%  {why}")

    # ---- 3. the vocabulary answer ------------------------------------------
    print("\n" + "=" * 74)
    print("3. THE NONE CLUSTERS -- what the vocabulary has no word for")
    print("=" * 74)
    tok_plugins = collections.defaultdict(set)
    tok_examples = collections.defaultdict(list)
    n_none = 0
    for p in props:
        plugin = p.get("plugin") or p.get("fp", "")[:8]
        for src in ("declines", "escalations"):
            for e in (p.get(src) or []):
                sems = [a.get("semantic") for a in (e.get("arms") or [])]
                if not sems or not all(s == "none" for s in sems):
                    continue
                n_none += 1
                nm = e.get("name") or e.get("control_name") or ""
                for t in set(tokens(nm)):
                    tok_plugins[t].add(plugin)
                    if len(tok_examples[t]) < 4:
                        tok_examples[t].append(f"{plugin[:18]}:{nm}")
    print(f"\n  {n_none:,} controls both arms called `none`, across {len(props)} maps\n")
    print(f"  {'token':20s} {'plugins':>7s}  examples")
    shown = 0
    for t, plugs in sorted(tok_plugins.items(), key=lambda kv: -len(kv[1])):
        if len(plugs) < 2 or shown >= args.clusters:
            continue
        shown += 1
        print(f"  {t:20s} {len(plugs):7d}  {', '.join(tok_examples[t][:3])[:64]}")
    print("\n  a token on many plugins is a SEMANTIC the dial set lacks;")
    print("  a token on one is that plugin's quirk.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
