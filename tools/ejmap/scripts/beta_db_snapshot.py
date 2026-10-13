#!/usr/bin/env python3
"""THE BETA DATABASE SNAPSHOT (Kathy, 13 Oct, HANDOVER_2026-10-13b pending 1).

A read-only snapshot of what Sean's cert folder knows, shipped inside the beta app so the wizard skips what EchoJay already has:

  <out>/*.json             every EXPORTED compressor profile (+ its tone check), every limiter's compressor pass that was exported with
                           its tone check passed, every EQ draft whose Phase B row is ok (ej_eq_profile/1) - full files, so rule B's
                           quick read-back check has the writes to check
  <out>/mapped.json        every other MEASURED Phase B draft as "already mapped" by plugin id + version + map fp (null when the cert
                           never recorded one): a draft counts when its file exists, its Phase B row's outcome is ok, and it is usable
                           (a saturation draft needs its drive - wizroute::draftUsable)
  <out>/categories.json    every settled category, plugin_id -> the server's name (and product name -> category when unambiguous):
                           Phase B work lists first (the server's categories), then the cert outcomes, then the settled signature reads
  <out>/multiband.json     the multiband uids (cert outcomes state multiband + the multiband Phase B rows)
  <out>/references/        the 290 agreed references (wizard_app/mock_db/references)
  <out>/snapshot.json      what it is, where from (the folder's NAME only), the counts

Gain / timing / frequency / material / sample-rate drafts are blocks of other profiles, not a category: they map nothing alone.
Nothing personal: every written file is checked for /Users/, the user's name and credential-like words; a hit refuses the snapshot.

    tools/ejmap/scripts/beta_db_snapshot.py "<cert folder>" <out folder>
"""
import collections, getpass, glob, json, os, shutil, socket, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REFS = os.path.join(HERE, "..", "wizard_app", "mock_db", "references")

# Phase B category folder -> (row sub-folder, the server's category); internal "multiband" stays an internal label (held by the wizard)
PHASEB = {
    "eq": "eq", "saturation": "saturation", "reverb": "reverb", "delay": "delay", "deesser": "de-esser", "limiter": "limiter",
    "limitercomp": "limiter", "strips": "channel_strip", "transient": "transient_shaper", "gate": "gate", "tuners": "pitch",
    "multiband": "multiband",
}
# the signature reads' labels -> what the wizard does with them (wizroute::afterSignature: dynamics is measured as a compressor)
SIGNATURE = {"dynamics": "compressor"}
OUTCOME_CAT = {"compressor": "compressor", "pitch": "pitch", "eq": "eq", "de-esser": "de-esser"}


def load(p):
    with open(p) as f:
        return json.load(f)


def pid_of(d):
    pl = d.get("plugin") or {}
    if pl.get("plugin_id"):
        return pl["plugin_id"], pl.get("name"), pl.get("version"), pl.get("map_fp")
    par = d.get("parent") or {}   # a strip draft names its plugin as the parent
    pid = par.get("plugin_id") or par.get("identity")
    return pid, par.get("name") or par.get("product"), par.get("version") or (pid.split("|")[2] if pid else None), par.get("map_fp")


def rows_of(cert, cat):
    rows = {}
    for f in glob.glob(os.path.join(cert, "phaseb", cat, "*.phaseb.json")):
        r = load(f)
        rows[r.get("identity")] = r
    return rows


def usable(cat, d):
    return d.get("drive") is not None if cat == "saturation" else True


def main(cert, out):
    if os.path.exists(out) and os.listdir(out):
        sys.exit(f"snapshot: {out} is not empty - give a fresh folder")
    os.makedirs(out, exist_ok=True)
    counts = collections.Counter()
    written = {}

    def put(name, src=None, data=None):
        if name in written:
            sys.exit(f"snapshot: two files would be named {name} ({written[name]} and {src})")
        written[name] = src or "generated"
        dst = os.path.join(out, name)
        if src:
            shutil.copyfile(src, dst)
        else:
            with open(dst, "w") as f:
                json.dump(data, f, indent=1, sort_keys=True)
                f.write("\n")

    have = {}   # plugin_id -> (kind, category)
    # 1. the exported compressor profiles, by the cert's outcomes (the profile file's NAME only: the record paths are Sean's)
    for o in load(os.path.join(cert, "outcomes.json")):
        if o.get("state") != "exported":
            continue
        prof = os.path.join(cert, "profiles", os.path.basename(o["profile"]))
        tc = os.path.join(cert, "profiles", os.path.basename(o["tonecheck"]))
        if not (os.path.isfile(prof) and os.path.isfile(tc)):
            sys.exit(f"snapshot: {o['product']} is exported but its profile / tone check is not in the folder")
        put(os.path.basename(prof), prof); put(os.path.basename(tc), tc)
        have[load(prof)["plugin"]["plugin_id"]] = ("profile", "compressor"); counts["compressor_profiles"] += 1
    # 2. a limiter's compressor pass: exported AND its tone check passed (wizroute::sendCompressorPass)
    lc = os.path.join(cert, "phaseb", "limitercomp")
    for f in glob.glob(os.path.join(lc, "outcome", "*.json")):
        for o in load(f):
            if o.get("state") != "exported":
                continue
            prof = os.path.join(lc, "profiles", os.path.basename(o["profile"]))
            tc = os.path.join(lc, "profiles", os.path.basename(o["tonecheck"]))
            if not (os.path.isfile(tc) and load(tc).get("pass_within_0_5_db") is True):
                counts["limitercomp_tonecheck_not_passed"] += 1
                continue
            put("limitercomp_" + os.path.basename(prof), prof); put("limitercomp_" + os.path.basename(tc), tc)
            have.setdefault(load(prof)["plugin"]["plugin_id"], ("profile", "limiter")); counts["limitercomp_profiles"] += 1
    # 3. the Phase B drafts: measured = the draft + an ok row + usable
    mapped = []
    for cat, server in PHASEB.items():
        if cat == "limitercomp":
            continue
        rows = rows_of(cert, cat)
        for f in sorted(glob.glob(os.path.join(cert, "phaseb", cat, "drafts", "*.json"))):
            d = load(f)
            pid, name, ver, fp = pid_of(d)
            row = rows.get(pid)
            if pid is None or row is None or row.get("outcome") != "ok":
                counts[f"draft_not_ok_{cat}"] += 1
                continue
            if not usable(cat, d):
                counts[f"draft_unusable_{cat}"] += 1
                continue
            if cat == "eq":
                put(os.path.basename(f), f)   # ej_eq_profile/1: a full profile, rule B can quick-check it
                have.setdefault(pid, ("profile", "eq")); counts["eq_profiles"] += 1
                continue
            mapped.append({"plugin_id": pid, "product": name or row.get("product"), "version": ver, "map_fp": fp, "category": server,
                           "measured": cat, "source": "Phase B draft (" + cat + ")"})
            have.setdefault(pid, ("draft", server)); counts[f"drafts_{cat}"] += 1
    put("mapped.json", data={"schema": "ej_wizard_mapped/1", "entries": sorted(mapped, key=lambda e: (e["plugin_id"], e["measured"]))})
    # 4. the settled categories: Phase B work lists (the server's categories) > cert outcomes > settled signature reads
    cats, src, conflicts = {}, {}, []

    def settle(pid, cat, where):
        if not pid or not cat:
            return
        if pid in cats:
            if cats[pid] != cat:
                conflicts.append(f"{pid}: {cats[pid]} ({src[pid]}) kept over {cat} ({where})")
            return
        cats[pid], src[pid] = cat, where

    names = collections.defaultdict(set)
    for cat, server in PHASEB.items():
        for pid, r in rows_of(cert, cat).items():
            settle(pid, server, "phaseb/" + cat); names[r.get("product")].add(cats.get(pid))
    for o in load(os.path.join(cert, "outcomes.json")):
        c = "multiband" if o.get("state") == "multiband" else OUTCOME_CAT.get(o.get("category"))
        settle(o.get("identity"), c, "outcomes"); names[o.get("product")].add(cats.get(o.get("identity")))
    for p in load(os.path.join(cert, "proposed_categories.json"))["products"]:
        if p.get("settled") and p.get("category"):
            c = SIGNATURE.get(p["category"], p["category"])
            settle(p.get("identity"), c, "signature"); names[p.get("product")].add(cats.get(p.get("identity")))
    catalogue = dict(cats)
    for n, s in names.items():
        if n and len(s) == 1 and None not in s and n not in catalogue:
            catalogue[n] = next(iter(s))
    put("categories.json", data=catalogue)
    counts["categories_by_id"] = len(cats); counts["categories_by_name"] = len(catalogue) - len(cats)
    by_src = collections.Counter(src.values())
    # 5. the multibands (uid, any version)
    mb = sorted({pid.split("|")[1].lower() for pid, c in cats.items() if c == "multiband"})
    put("multiband.json", data={"schema": "ej_wizard_multiband/1", "uids": mb}); counts["multiband_uids"] = len(mb)
    # 6. the references
    rd = os.path.join(out, "references"); os.makedirs(rd)
    for f in sorted(glob.glob(os.path.join(REFS, "*.reference.json"))):
        shutil.copyfile(f, os.path.join(rd, os.path.basename(f))); counts["references"] += 1
    kinds = collections.Counter(k for k, _ in have.values())
    put("snapshot.json", data={
        "schema": "ej_wizard_db_snapshot/1", "source": os.path.basename(os.path.normpath(cert)) + " (Sean's cert folder, read-only)",
        "read_only": True, "already_mapped_plugin_ids": len(have), "by_kind": dict(kinds), "counts": dict(counts),
        "categories_by_source": dict(by_src), "category_conflicts": conflicts,
        "rule": "already mapped = an exported profile (compressor, limiter's compressor pass with its tone check passed), an EQ draft or another "
                "category's Phase B draft with an ok row (saturation: with its drive); keyed by plugin id + version, and map fp where the cert recorded one"})
    # 7. nothing personal
    user, host = getpass.getuser(), socket.gethostname().split(".")[0]
    bad = []
    for root, _, files in os.walk(out):
        for fn in files:
            t = open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
            for k in ["/Users/", "\"token\"", "mapper_token", "api_key", "apikey", "Bearer ", "Authorization", "password"] + \
                     ([user] if len(user) >= 3 else []) + ([host] if len(host) >= 3 else []):
                if k.lower() in t.lower():
                    bad.append(f"{fn}: {k}")
    if bad:
        sys.exit("snapshot: REFUSED - personal / credential-like text:\n  " + "\n  ".join(bad[:20]))
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(out) for f in fs)
    nfiles = sum(len(fs) for _, _, fs in os.walk(out))
    print(f"snapshot: {out}: {nfiles} files, {size} bytes ({size / 1048576:.1f} MiB)")
    print(f"  already mapped: {len(have)} plugin id(s) {dict(kinds)}")
    for k in sorted(counts):
        print(f"  {k}: {counts[k]}")
    print(f"  categories by source: {dict(by_src)}; {len(conflicts)} conflict(s) (first source kept)")
    for c in conflicts:
        print("    " + c)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
