#!/usr/bin/env python3
"""THE BAR'S WEIGHTS (Kathy, 14 Oct): how much work each plugin's measuring is, from Sean's real runs.

    expected_work.py <Sean's cert folder (read only)> <database folder>   -> <database folder>/expected_work.json

For each plugin (the AU uid, lower-case hex: the same across versions) and each wizard kind (compressor, eq, deesser, limiter,
limitercomp, saturation, gate, transient, tuners, reverb, delay, strips), Sean's seconds and his probe-process count:
  - compressor: the certification's run.jsonl (every probe call of the sweep, ms and count, by the file's uid);
  - the rest: phaseb/<category>/*.phaseb.json "seconds" of an "ok" row, and the row's raw traces (one per probe process).
"_median" holds each kind's median over Sean's rows, the weight of a plugin he never measured. The wizard's progress bar weights
each plugin by these seconds (EjmapWizardProgress.h, wizprog::ExpectedWork), so a 20-minute reverb moves the bar like one.
Sean's folder is only read.
"""
import glob, gzip, json, os, re, statistics, sys
from collections import defaultdict

PHASEB_KIND = {"eq": "eq", "deesser": "deesser", "limiter": "limiter", "limitercomp": "limitercomp", "saturation": "saturation",
               "gate": "gate", "transient": "transient", "tuners": "tuners", "reverb": "reverb", "delay": "delay", "strips": "strips"}

def main(cert, db):
    rows = defaultdict(dict)   # uid -> kind -> {seconds, procs}
    runlog = os.path.join(cert, "run.jsonl")
    if os.path.isfile(runlog):
        acc = defaultdict(lambda: [0.0, 0])
        for line in open(runlog):
            try: r = json.loads(line)
            except ValueError: continue
            m = re.match(r"AudioUnit_([0-9a-f]+)_", r.get("file") or "")
            if not m: continue
            a = acc[m.group(1)]; a[0] += float(r.get("ms") or 0) / 1000.0; a[1] += 1
        for uid, (s, n) in acc.items(): rows[uid]["compressor"] = {"seconds": round(s, 1), "procs": n}
    for cat, kind in PHASEB_KIND.items():
        d = os.path.join(cert, "phaseb", cat)
        procs = defaultdict(int)
        for f in glob.glob(os.path.join(d, "raw", "*")): procs[os.path.basename(f).split("_")[1].lower()] += 1
        for f in glob.glob(os.path.join(d, "*.phaseb.json")):
            r = json.load(open(f))
            if r.get("outcome") != "ok" or not r.get("seconds"): continue
            uid = r["identity"].split("|")[1].lower()
            rows[uid][kind] = {"seconds": round(float(r["seconds"]), 1), "procs": procs.get(uid, 0)}
    median = {}
    for kind in ["compressor"] + list(PHASEB_KIND.values()):
        xs = [v[kind] for v in rows.values() if kind in v]
        if xs: median[kind] = {"seconds": round(statistics.median(x["seconds"] for x in xs), 1),
                               "procs": int(statistics.median(x["procs"] for x in xs)), "n": len(xs)}
    out = {"schema": "ej_wizard_expected_work/1", "source": "Sean's cert 5 (compressor run.jsonl + Phase B rows)",
           "_median": median, "plugins": {u: rows[u] for u in sorted(rows)}}
    path = os.path.join(db, "expected_work.json")
    with open(path, "w") as fh: json.dump(out, fh, indent=1, sort_keys=True); fh.write("\n")
    print(f"{path}: {len(rows)} plugin(s); medians " + ", ".join(f"{k} {v['seconds']} s / {v['procs']}" for k, v in median.items()))

if __name__ == "__main__":
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
