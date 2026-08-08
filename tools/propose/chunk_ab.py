#!/usr/bin/env python3
"""Does chunking change what the arms say?

    python3 chunk_ab.py --n 5 --scratch /tmp/ab

16% of the corpus -- 8,407 controls across the 11 giants -- was proposed in
chunks, so each arm judged a control with only ~200 siblings in view instead of
the whole surface. The 98.7% hold-out was measured on whole-plugin prompts and
therefore says nothing about those rows. That is the one unmeasured number in an
otherwise measured pipeline, and it sits in the plugins with the most surface.

THE TEST NEEDS A CONTROL ARM, and this is the whole design. The arms are
stochastic: two identical whole-plugin runs will disagree with each other on
some controls. Comparing whole-vs-chunked alone would charge every one of those
disagreements to chunking. So each plugin is proposed THREE times:

    whole_1     the reference
    whole_2     same prompt, same settings -- measures the run-to-run floor
    chunked     the same controls, split into requests

and the number that matters is the DIFFERENCE OF DIFFERENCES:

    A/B disagreement  -  A/A disagreement

If chunking changes nothing, those two are the same and the answer is "within
noise". If chunking costs something, A/B is worse than A/A by that much.

It compares VERDICTS, not just accepts, because a control moving from accepted
to escalated is a different cost from a control changing its semantic: the first
sends work to a human, the second ships a wrong dial.

Reads maps from the real root and writes NOTHING to it -- every proposal goes to
--scratch. The ledger is the corpus.
"""
import argparse, collections, concurrent.futures, glob, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import propose as P


def verdicts(doc):
    """One row per control: what the pipeline decided about it."""
    out = {}
    for p in doc.get("params") or []:
        out[p.get("control_name")] = ("accept", p.get("kind"))
    for e in doc.get("escalations") or []:
        out[e.get("name")] = ("escalate", (e.get("why") or "").split(":")[0])
    for d in doc.get("declines") or []:
        out[d.get("control_name")] = ("decline", "")
    out.pop(None, None)
    return out


def compare(a, b):
    """Returns (n, same, changes) over the controls both runs decided."""
    keys = set(a) & set(b)
    changes = [(k, a[k], b[k]) for k in sorted(keys) if a[k] != b[k]]
    return len(keys), len(keys) - len(changes), changes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--n", type=int, default=5)
    ap.add_argument("--min", type=int, default=150)
    # Below the truncation point measured on 7 Aug: the whole-plugin arm has to
    # actually succeed, or the test compares chunking against a failure.
    ap.add_argument("--max", type=int, default=380)
    ap.add_argument("--chunk", type=int, default=100)
    ap.add_argument("--effort", default="high")
    ap.add_argument("--workers", type=int, default=2)
    args = ap.parse_args()

    import anthropic, openai
    clients = {"anthropic": anthropic.Anthropic(), "openai": openai.OpenAI()}

    picks = []
    for f in sorted(glob.glob(os.path.join(args.root, "maps", "*.json"))):
        d = json.load(open(f))
        n = len(d.get("controls") or {})
        if args.min <= n <= args.max:
            picks.append((f, d["identity"]["name"], n))
    # Evenly spaced through the eligible set rather than the first N, so the
    # sample is not all one vendor's alphabetical run.
    step = max(1, len(picks) // args.n)
    picks = picks[::step][:args.n]
    print(f"{len(picks)} plugin(s), {sum(n for _, _, n in picks):,} controls, "
          f"x3 runs, chunk={args.chunk}")
    for _, nm, n in picks:
        print(f"    {nm:38} {n:4} controls -> {-(-n // args.chunk)} chunks")

    arms = [("whole_1", 0), ("whole_2", 0), ("chunked", args.chunk)]
    for label, _ in arms:
        os.makedirs(os.path.join(args.scratch, label, "proposals"), exist_ok=True)

    jobs = [(f, label, ch) for f, _, _ in picks for label, ch in arms]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(P.run_one, f,
                          os.path.join(args.scratch, label, "proposals"),
                          clients, False, label, args.effort, ch): (f, label)
                for f, label, ch in jobs}
        for fut in concurrent.futures.as_completed(futs):
            f, label = futs[fut]
            try:
                name, a, e, note = fut.result()
                print(f"  {label:8} {a:3d} accepted  {e:3d} to review   {name}   [{note}]",
                      flush=True)
            except Exception as exc:                            # noqa: BLE001
                print(f"  !! {label} {os.path.basename(f)}: {exc}", flush=True)

    print("\n" + "=" * 74)
    print("DIFFERENCE OF DIFFERENCES -- chunking against the run-to-run floor")
    print("=" * 74)
    tot = collections.Counter()
    accept_changes = []
    for f, nm, n in picks:
        fp = json.load(open(f))["fp"]
        docs = {}
        for label, _ in arms:
            p = os.path.join(args.scratch, label, "proposals", fp + ".json")
            docs[label] = verdicts(json.load(open(p))) if os.path.exists(p) else None
        if any(v is None for v in docs.values()):
            print(f"  {nm}: a run did not produce a proposal -- excluded")
            continue
        n_aa, same_aa, ch_aa = compare(docs["whole_1"], docs["whole_2"])
        n_ab, same_ab, ch_ab = compare(docs["whole_1"], docs["chunked"])
        tot["n_aa"] += n_aa; tot["same_aa"] += same_aa
        tot["n_ab"] += n_ab; tot["same_ab"] += same_ab
        print(f"  {nm:34} A/A {same_aa:4}/{n_aa:4} = {100*same_aa/max(1,n_aa):5.1f}%"
              f"   A/B {same_ab:4}/{n_ab:4} = {100*same_ab/max(1,n_ab):5.1f}%")
        for k, a, b in ch_ab:
            if a[0] == "accept" or b[0] == "accept":
                accept_changes.append((nm, k, a, b))

    aa = 100 * tot["same_aa"] / max(1, tot["n_aa"])
    ab = 100 * tot["same_ab"] / max(1, tot["n_ab"])
    print(f"\n  A/A  two whole runs agree   {tot['same_aa']:5,}/{tot['n_aa']:5,} = {aa:5.2f}%"
          "   <- the floor: how much the arms move on their own")
    print(f"  A/B  whole vs chunked       {tot['same_ab']:5,}/{tot['n_ab']:5,} = {ab:5.2f}%")
    print(f"\n  CHUNKING'S OWN COST: {aa - ab:+.2f} percentage points")
    print("  (positive = chunking disagrees more than two identical runs do;"
          "\n   at or below zero = the difference is run-to-run noise, not chunking)")

    print(f"\n  every A/B change that touches an ACCEPT ({len(accept_changes)}) -- "
          "a wrong dial, not a queue item:")
    for nm, k, a, b in accept_changes:
        print(f"    {nm}: {k!r}   whole={a[0]}/{a[1] or '-'}   chunked={b[0]}/{b[1] or '-'}")
    if not accept_changes:
        print("    none")


if __name__ == "__main__":
    sys.exit(main() or 0)
