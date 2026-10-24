#!/usr/bin/env python3
"""
make_real_fixture_pack.py - the REAL results for MOCK_FOR_B (Kathy's ruling 2, 22 Oct): a pack the contract test sends through the real
client (RoundTripTest, scenario 18_real_results, EJ_REAL_FIXTURES=<pack>) so B's fixtures are real wizard results, not hand-made ones.

  python3 make_real_fixture_pack.py --engine <a run's engine folder> --selection <selection.json> --maps <full maps folder>
                                    [--extra <engine folder> --extra-names "A,B"] --out <pack>

The pack: manifest.json (the selected entries, as the engine packed them), bundle/ (their files: profiles, tone checks, drafts, the
needs_category / held entries with the plugin's FULL map, the crash report, the bridge read), measured_at.json, identities.json and
request.json (the catalogue check). Read only: nothing in the run folders is written.

measured_at (contract v1.1 §4) for a run packed before measured_at existed comes from that run's own records, in this order: the plugin's
Phase B row time ("at"); else its last line in loads.log; else the run's first loads.log time (a plugin never loaded).
measure_started_at (24 Oct, REQUIRED): that Phase B row's start ("started_at", else "at" less "seconds"); anything else its measured_at.
"""
import argparse, glob, json, os, plistlib, shutil
from datetime import datetime, timedelta


def iso_local(stamp):
    """'20261020T135139' (local) -> ISO 8601 with the Mac's offset"""
    try:
        return datetime.strptime(stamp[:15], "%Y%m%dT%H%M%S").astimezone().isoformat(timespec="seconds")
    except ValueError:
        return None


def vendors():
    """product name -> vendor, from every installed AU's Info.plist ("Vendor: Product")"""
    out = {}
    for root in ("/Library/Audio/Plug-Ins/Components", os.path.expanduser("~/Library/Audio/Plug-Ins/Components")):
        for p in glob.glob(os.path.join(root, "*.component/Contents/Info.plist")):
            try:
                for c in plistlib.load(open(p, "rb")).get("AudioComponents", []):
                    n = c.get("name", "")
                    if ": " in n:
                        v, prod = n.split(": ", 1)
                        out.setdefault(prod, v)
            except Exception:
                pass
    return out


def times(engine):
    rows, starts = {}, {}
    for f in glob.glob(os.path.join(engine, "phaseb", "*", "*.phaseb.json")):
        r = json.load(open(f))
        t = iso_local(r.get("at", ""))
        if t and r.get("product") and t >= rows.get(r["product"], t):
            rows[r["product"]] = t
            # measure_started_at (Sean / B, 24 Oct): that row's child's start - "started_at" from 24 Oct on, else "at" less "seconds"
            starts[r["product"]] = r.get("started_at") or (datetime.fromisoformat(t) - timedelta(seconds=float(r.get("seconds") or 0))).isoformat(timespec="seconds")
    loads, first = {}, None
    for line in open(os.path.join(engine, "loads.log")):
        parts = line.rstrip("\n").split("\t")
        if len(parts) >= 3:
            first = first or parts[0]
            loads[parts[2]] = parts[0]
    rows["_starts"] = starts
    return rows, loads, first


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True); ap.add_argument("--selection", required=True); ap.add_argument("--maps", required=True)
    ap.add_argument("--extra"); ap.add_argument("--extra-names", default=""); ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out, B = a.out, os.path.join(a.out, "bundle")
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(B, "maps"))
    sel = json.load(open(a.selection))
    full = {json.load(open(f))["fp"]: f for f in glob.glob(os.path.join(a.maps, "*.json"))}
    byname = {json.load(open(f))["identity"]["name"]: json.load(open(f)) for f in full.values()}
    src = os.path.join(a.engine, "contribution")
    rows, loads, first = times(a.engine)
    ven = vendors()
    measured, started, ids, plugins = {}, {}, {}, []

    def take(rel, base=src):
        d = os.path.join(B, rel); os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copy(os.path.join(base, rel), d)

    def note(e, rows=rows, loads=loads, first=first):
        pid, prod = e.get("plugin_id"), e.get("product", "")
        measured[pid] = rows.get(prod) or loads.get(prod) or first
        started[pid] = rows["_starts"][prod] if rows.get(prod) else measured[pid]   # measured nothing on record: its measured_at
        ids[pid] = {"vendor": ven.get(prod, ""), "product": prod}
        fp = e.get("map_fp")
        plugins.append({"plugin_id": pid, "name": prod, "vendor": ven.get(prod, ""), "version": pid.split("|")[2], "format": pid.split("|")[0], "type": "aufx",
                        "fingerprint": fp if fp else None})

    def attach_map(e):
        m = byname.get(e["product"])
        if m is None:
            raise SystemExit("no full map for %s" % e["product"])
        if e.get("map_fp") and e["map_fp"] != m["fp"]:
            raise SystemExit("%s: the run's map_fp %s is not this Mac's full map %s" % (e["product"], e["map_fp"][:12], m["fp"][:12]))
        rel = "maps/%s.json" % m["fp"]; json.dump(m, open(os.path.join(B, rel), "w"))
        return rel, m

    man = {"schema": "ej_contribution/0", "note": "REAL results from the 20 Oct wizard run (one Mac), re-packed for the contract v1.1 fixtures"}
    for k, v in sel.items():
        man[k] = []
        for e in v:
            e = dict(e)
            for f in ("profile", "tone_check"):
                if e.get(f): take(e[f])
            for d in e.get("drafts") or []:
                take(d)
            if k == "profiles":
                e["map_file"], _ = attach_map(e)
            if k in ("needs_category",) and e.get("file"):
                entry = json.load(open(os.path.join(src, e["file"])))
                rel, m = attach_map(e); entry["map"] = m; e["map_fp"] = m["fp"]
                os.makedirs(os.path.dirname(os.path.join(B, e["file"])), exist_ok=True); json.dump(entry, open(os.path.join(B, e["file"]), "w"))
            if k == "held":
                rel, m = attach_map(e); e["map"] = m; e["map_fp"] = m["fp"]
            if k == "crashed" and e.get("crash_report"):
                take(e["crash_report"])
            if k == "bridge_reads" and e.get("file"):
                take(e["file"])
            if k != "crashed":
                note(e)
            man[k].append(e)
    if a.extra:   # the v1.1 drafts (filter, pitch shift): their own run's records
        xs = os.path.join(a.extra, "contribution"); xm = json.load(open(os.path.join(xs, "manifest.json")))
        xrows, xloads, xfirst = times(a.extra)
        for e in xm.get("profiles", []):
            if e["product"] not in a.extra_names.split(","):
                continue
            e = dict(e)
            for d in e.get("drafts") or []:
                take(d, xs)
            if e.get("map_file"):
                take(e["map_file"], xs)
            note(e, xrows, xloads, xfirst); man["profiles"].append(e)
    # the crash names only its product: its plugin_id from the run's own rows
    state = json.load(open(os.path.join(a.engine, "wizard_state.json")))
    pid_of = {r["product"]: r.get("plugin_id") for r in state["plugins"] if r.get("plugin_id")}
    for e in man.get("crashed", []):
        pid = pid_of.get(e["product"])
        if pid:
            measured[pid] = loads.get(e["product"]) or first; started[pid] = measured[pid]; ids[pid] = {"vendor": ven.get(e["product"], ""), "product": e["product"]}
            plugins.append({"plugin_id": pid, "name": e["product"], "vendor": ven.get(e["product"], ""), "version": pid.split("|")[2], "format": pid.split("|")[0], "type": "aufx", "fingerprint": None})
    json.dump(man, open(os.path.join(out, "manifest.json"), "w"), indent=1)
    json.dump(measured, open(os.path.join(out, "measured_at.json"), "w"), indent=1)
    json.dump(started, open(os.path.join(out, "measure_started_at.json"), "w"), indent=1)
    json.dump(ids, open(os.path.join(out, "identities.json"), "w"), indent=1)
    json.dump({"plugins": plugins}, open(os.path.join(out, "request.json"), "w"), indent=1)
    json.dump(pid_of, open(os.path.join(out, "pid_of_product.json"), "w"), indent=1)
    print("pack: %d plugin(s); %s -> %s" % (len(plugins), {k: len(v) for k, v in man.items() if isinstance(v, list)}, out))


if __name__ == "__main__":
    main()
