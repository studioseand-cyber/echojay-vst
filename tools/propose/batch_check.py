#!/usr/bin/env python3
"""The stop-and-look check for a sweep campaign: audit + eyes.

    python3 batch_check.py --since 2026-08-07T09:00:00 \
                           --captures ~/Library/ejmap/overnight

Two outputs, in this order, because they answer different questions:

  1. audit_maps over ONLY the maps this batch wrote, checked against an
     INDEPENDENT capture record -- a snapshot taken before the run started,
     never the run's own logs. A run whose leak poisons both its maps and its
     evidence audits itself clean; a snapshot cannot be poisoned by a run that
     had not started yet. Coverage is printed per check, as always.

  2. A SPOT CHECK FOR EYES, NOT COUNTERS: N random maps from the batch, each
     printed as the plugin's name, vendor, category and parameter count beside
     every control name it claims. The last two silent failures -- half a sweep
     carrying the previous plugin's surface, a fallback laundering foreign
     controls -- were both plausible-looking output that a counter reported as
     success. A human reading "UAD Korg SDD-3000 (delay): Ch64Bass, Ch66Vol"
     catches in two seconds what 19/19 did not.

The audit root is assembled in a temp directory: the batch's maps, the
snapshot's captures, the live scan-cache for identity joins. Nothing in the
live root is touched.
"""
import argparse, datetime, glob, json, os, random, shutil, subprocess, sys, tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--since", required=True,
                    help="ISO time; maps modified after this are 'the batch'")
    ap.add_argument("--captures", required=True,
                    help="directory holding the INDEPENDENT captures-*.jsonl snapshot")
    ap.add_argument("--sample", type=int, default=10)
    ap.add_argument("--seed", type=int, default=None,
                    help="fix the random sample, for re-reading the same ten")
    args = ap.parse_args()

    since = datetime.datetime.fromisoformat(args.since).timestamp()
    batch = [f for f in glob.glob(os.path.join(args.root, "maps", "*.json"))
             if os.path.getmtime(f) > since]
    print(f"batch: {len(batch)} map(s) written since {args.since}")
    if not batch:
        return 0

    snap = glob.glob(os.path.join(os.path.expanduser(args.captures), "captures-*.jsonl"))
    print(f"independent record: {len(snap)} capture file(s) from {args.captures}")
    if not snap:
        print("REFUSING: no snapshot captures -- an audit against nothing is not an audit")
        return 2

    # ---- 1. the audit, in an assembled root --------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "maps"))
        for f in batch:
            shutil.copy(f, os.path.join(tmp, "maps"))
        for f in snap:
            shutil.copy(f, tmp)
        sc = os.path.join(args.root, "scan-cache.xml")
        if os.path.exists(sc):
            shutil.copy(sc, tmp)

        rc = subprocess.run(
            [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                          "audit_maps.py"), "--root", tmp]).returncode

    # ---- 2. the spot check, for eyes ----------------------------------------
    print("\n" + "=" * 74)
    print(f"SPOT CHECK -- {min(args.sample, len(batch))} random map(s). READ the names "
          "against what the plugin IS.")
    print("=" * 74)
    rng = random.Random(args.seed)
    for f in sorted(rng.sample(batch, min(args.sample, len(batch)))):
        d = json.load(open(f))
        i = d["identity"]
        controls = sorted((d.get("controls") or {}).keys())
        print(f"\n  {i['name']}   ({i.get('vendor','?')}, {d.get('category','?')}, "
              f"{i.get('param_count','?')} params -> {len(controls)} controls)")
        line = "     "
        for c in controls:
            if len(line) + len(c) > 76:
                print(line.rstrip(", ") if False else line)
                line = "     "
            line += c + ", "
        if line.strip():
            print(line.rstrip(", "))

    print(f"\naudit exit: {rc}  "
          + ("(CLEAN)" if rc == 0 else "(A MAP IS IMPLICATED -- read the verdict above)"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
