#!/usr/bin/env python3
"""
ratio_law_salvage.py - SALVAGE A RATIO LAW RUN FROM AN EARLIER PACK (Kathy, 19 Oct 2026).

Sean ran the pack built before the placement fix (cert 924b9ff3) and the convention fix: some writes landed away from the ratio
aimed at, some landed where nothing was read back, and T was stored in the served sine-RMS convention. This:
  1. copies every <stem>.ratio_law.json from Sean's results into <out>/rescored/ (his files are never written);
  2. re-scores each copy with the FIXED scorer (`ejmap --ratio-law-score`): every setting by the plugin's own read-back text, a
     landing with no read-back left unscored, T converted, the curves in sine RMS dBFS (level_ref) - the shape B accepts;
  3. works out, per plugin, which target settings have NO read landing near them (the fixed pack's targets: the measured, the
     lowest, 2:1, 4:1, the highest, every one snapped to the nearest READ ratio - so a target counts as covered when a read
     landing sits within 10 % of it, or is the same named/limit position);
  4. writes <out>/products_topup.txt (only the plugins with a missing setting - the fixed pack runs whole plugins, so each listed
     plugin is re-run once and its good settings are measured again alongside) and <out>/SALVAGE.md (what was kept, what is missing).
A run file with no curves (ej_ratio_law_run/1: the T+6/9/12 pack) lists the plugin whole: its settings were never read as curves.

    python3 ratio_law_salvage.py <sean's ratio_law folder> <out folder> [--ejmap <path to ejmap>] [--list products_sean_mac.txt]
Loads nothing, sends nothing.
"""
import json, math, os, shutil, subprocess, sys

COVER = 0.10          # a read landing within 10 % of a target covers it
INF = 1000.0

def ratios_targets(run):
    """the fixed pack's targets for this plugin, from what its read-backs say about the dial (lowest / highest READ) and the profile"""
    read = sorted({float(x["labelled_ratio"]) for x in run.get("runs", []) if "labelled_ratio" in x})
    out = {"the measured": float(run.get("measured_ratio", 0.0))}
    if read:
        out["the lowest"] = read[0]
        out["the highest"] = read[-1]
    for r in (2.0, 4.0):
        if not read or (read[0] - 1e-9 <= r <= read[-1] + 1e-9):   # never past the dial's read ends
            out["%g:1" % r] = r
    return out, read

def covered(target, read):
    for r in read:
        if (target >= INF) == (r >= INF) and (target >= INF or abs(math.log(r / target)) <= math.log(1.0 + COVER)):
            return r
    return None

def main(argv):
    if len(argv) < 3:
        print(__doc__); return 2
    src, out = argv[1], argv[2]
    ejmap = "ejmap"; listf = None
    for i, a in enumerate(argv):
        if a == "--ejmap" and i + 1 < len(argv): ejmap = argv[i + 1]
        if a == "--list" and i + 1 < len(argv): listf = argv[i + 1]
    resc = os.path.join(out, "rescored"); os.makedirs(resc, exist_ok=True)
    files = sorted(f for f in os.listdir(src) if f.endswith(".ratio_law.json"))
    topup, lines = [], []
    seen_products = set()
    for f in files:
        dst = os.path.join(resc, f); shutil.copyfile(os.path.join(src, f), dst)
        before = json.load(open(dst))
        schema = before.get("schema", "")
        r = subprocess.run([ejmap, "--ratio-law-score", dst], capture_output=True, text=True)
        after = json.load(open(dst))
        product = after.get("product", f); seen_products.add(product)
        rl = after.get("ratio_law", {})
        targets, read = ratios_targets(after)
        missing = [w + " (" + ("inf" if t >= INF else "%g" % t) + ":1)" for w, t in targets.items() if covered(t, read) is None]
        unread = rl.get("runs_unread", 0)
        whole = schema == "ej_ratio_law_run/1"
        if whole: missing = ["every setting: this run file has no curves (the T+6/9/12 pack)"]
        if missing: topup.append(product)
        lines.append("| %s | %s | %s | %d | %s | %s |" % (product, schema or "?", ", ".join(("inf" if x >= INF else "%g" % x) for x in read) or "none",
                                                       unread, rl.get("status", "?"), "; ".join(missing) or "nothing - kept as is"))
        if r.returncode != 0: lines[-1] += " (scorer exit %d)" % r.returncode
    absent = []
    if listf and os.path.exists(listf):
        want = [l.split("#")[0].strip() for l in open(listf) if l.split("#")[0].strip()]
        absent = [w for w in want if w not in seen_products]
        topup += [a for a in absent if a not in topup]
    with open(os.path.join(out, "products_topup.txt"), "w") as fh:
        fh.write("# Top-up for the FIXED pack (19 Oct): only plugins with a setting missing after the salvage, or with no result at all\n")
        for p in topup: fh.write(p + "\n")
    with open(os.path.join(out, "SALVAGE.md"), "w") as fh:
        fh.write("# Ratio law salvage of Sean's run\n\nRe-scored copies are in `rescored/`; Sean's files were not written.\n\n")
        fh.write("| plugin | run file | ratios read back | landings unread | status | missing (goes in the top-up) |\n|---|---|---|---|---|---|\n")
        fh.write("\n".join(lines) + "\n")
        if absent: fh.write("\n**No result file at all (in the top-up):** " + ", ".join(absent) + "\n")
        fh.write("\n**Top-up: %d plugin(s)** in `products_topup.txt`. Run with the fixed pack: `bash ratio_law.sh products_topup.txt`.\n" % len(topup))
    print("SALVAGE: %d file(s) re-scored, %d plugin(s) in the top-up -> %s" % (len(files), len(topup), out))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
