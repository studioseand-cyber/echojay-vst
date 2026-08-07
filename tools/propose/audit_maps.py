#!/usr/bin/env python3
"""Audit maps for a control surface that belongs to a DIFFERENT plugin.

    python3 audit_maps.py [--root ~/Library/ejmap]

WHAT THIS CATCHES. Per-plugin wizard state that survives into the next plugin
means a map can be written carrying the PREVIOUS plugin's controls under this
plugin's fingerprint. The map is structurally valid, passes the mouth gate, and
is wrong -- it would dial an EQ's shelf when someone asks a compressor for
something. Editing cannot fix it, because nothing in it is a measurement OF this
plugin.

FOUR CHECKS, strongest first. Only the first is a proof.

  1. INDEX DISAGREEMENT (proof, needs nothing external)
     `params` is keyed by semantic and `controls` by name, but both carry the
     parameter INDEX. An index that appears in both must carry the same name --
     it is one parameter on one plugin. When index 3 is "Output Trim" in params
     and "Low Shelf Level" in controls, the map contradicts itself, and one of
     the two was measured somewhere else.

  2. IDENTICAL CONTROL SET on two different products (strong)
     Two plugins with byte-identical control names, ranges and anchors did not
     measure the same thing twice. Mono/stereo variants of ONE product are
     excluded -- those legitimately match.

  3. INDEX OUT OF RANGE (proof, when param_count is present)
     A control at index 7 on a plugin with 4 parameters was swept elsewhere.

  4. NO OVERLAP AT ALL (weak, reported separately and never acted on alone)
     A map whose params and controls share no index is not evidence of anything
     -- a wizard can legitimately claim every index in one tier. Reported only
     so a reader can see the population the first check cannot see into.

WHY THE FIRST CHECK IS THE ONE THAT MATTERS. It needs no second map, no server,
no live plugin and no judgement. A single map is enough, and the disagreement is
arithmetic.
"""
import argparse, collections, glob, json, os, sys


def observed_names(root):
    """index -> name, per plugin_id, MEASURED LIVE and written by the capture
    log while the mapper worked. This is the independent source: the map says
    what it believes, and these rows say what the plugin actually reported.

    Without it the self-contradiction check sees only maps whose two tiers
    happen to share an index -- 1 of 40 here, because the tiers are DISJOINT by
    design (122 of 125 param indices appear in no control). A check with 2.5%
    coverage is not an audit.
    """
    obs, unreadable = collections.defaultdict(dict), 0
    for f in glob.glob(os.path.join(root, "captures-*.jsonl")):
        for line in open(f):
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except Exception:                                    # noqa: BLE001
                unreadable += 1                 # a few rows are pretty-printed
                continue
            pid = d.get("plugin_id")
            if not pid:
                continue
            for ik, nk in (("index", "param_name"),
                           ("resolved_index", "proposed_name"),
                           ("proposed_index", "proposed_name")):
                i, n = d.get(ik), d.get(nk)
                if isinstance(i, int) and i >= 0 and isinstance(n, str) and n:
                    obs[pid].setdefault(i, set()).add(n)
            if isinstance(d.get("names"), list) and isinstance(d.get("indices"), list):
                for i, n in zip(d["indices"], d["names"]):
                    if isinstance(i, int) and isinstance(n, str) and n:
                        obs[pid].setdefault(i, set()).add(n)
    return obs, unreadable


def plugin_ids_by_identity(root):
    """The scan cache joins a map's identity to the plugin_id the capture log
    keys on. format|hex(uid)|version, exactly as identityKeyForDescription."""
    import xml.etree.ElementTree as ET
    out = {}
    path = os.path.join(root, "scan-cache.xml")
    if not os.path.exists(path):
        return out
    for p in ET.parse(path).getroot().iter("PLUGIN"):
        a = p.attrib
        fmt = "AudioUnit" if a["file"].startswith("AudioUnit:") else "VST3"
        out[f"{fmt}|{a.get('uniqueId','').lower()}|{a.get('version','')}"] = a["file"]
    return out


def load_maps(root):
    out = []
    for f in sorted(glob.glob(os.path.join(root, "maps", "*.json"))):
        try:
            out.append((f, json.load(open(f))))
        except Exception as e:                                   # noqa: BLE001
            print(f"  UNREADABLE {os.path.basename(f)}: {e}")
    return out


def index_disagreements(d):
    """Check 1. Every index claimed by both tiers, with conflicting names."""
    by_index = collections.defaultdict(dict)
    for sem, p in (d.get("params") or {}).items():
        i = p.get("index")
        if i is not None:
            by_index[i]["params"] = (sem, p.get("name"))
    for name, c in (d.get("controls") or {}).items():
        for i in ([c["index"]] if c.get("index") is not None else (c.get("indices") or [])):
            by_index[i]["controls"] = (name, c.get("name"))

    bad = []
    for i, side in sorted(by_index.items()):
        if "params" in side and "controls" in side:
            pname = side["params"][1] or side["params"][0]
            cname = side["controls"][1] or side["controls"][0]
            if pname != cname:
                bad.append((i, pname, cname))
    return bad, sum(1 for s in by_index.values() if len(s) == 2)


def control_signature(d):
    """Names AND measurements. Two plugins cannot measure the same anchors."""
    sig = []
    for name, c in sorted((d.get("controls") or {}).items()):
        sig.append((name, tuple(c.get("range") or ()), c.get("unit"),
                    len(c.get("anchors") or [])))
    return tuple(sig)


def product_of(d):
    """Mono/stereo variants of one product legitimately share a surface."""
    n = d["identity"]["name"]
    for suf in (" (m)", " (s)", " (mono)", " (stereo)"):
        if n.lower().endswith(suf):
            n = n[: -len(suf)]
            break
    return (n.strip().lower(), (d["identity"].get("vendor") or "").strip().lower())


def out_of_range(d):
    n = d["identity"].get("param_count") or 0
    if n <= 0:
        return []
    bad = []
    for name, c in (d.get("controls") or {}).items():
        for i in ([c["index"]] if c.get("index") is not None else (c.get("indices") or [])):
            if i >= n:
                bad.append((name, i, n))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    args = ap.parse_args()

    maps = load_maps(args.root)
    print(f"{len(maps)} map(s) in {args.root}/maps\n")

    obs, unreadable = observed_names(args.root)
    ids = plugin_ids_by_identity(args.root)
    print(f"independent evidence: {sum(len(v) for v in obs.values())} index->name pairs "
          f"measured live across {len(obs)} plugin(s)"
          + (f"  ({unreadable} capture rows unreadable)" if unreadable else "") + "\n")

    print("=" * 74)
    print("CHECK 0 -- a control name the PLUGIN never reported at that index  (PROOF)")
    print("=" * 74)
    checked0, bad0 = 0, []
    for f, d in maps:
        i = d["identity"]
        key = f"{i.get('format')}|{(i.get('uid') or '').lower()}|{i.get('version')}"
        pid = ids.get(key)
        seen = obs.get(pid or "", {})
        if not seen:
            continue
        checked0 += 1
        wrong = []
        for name, c in (d.get("controls") or {}).items():
            idx = c.get("index")
            if idx is None or idx not in seen:
                continue
            if name not in seen[idx]:
                wrong.append((idx, name, sorted(seen[idx])))
        if wrong:
            bad0.append((d, wrong))
            print(f"\n  {i['name']}   fp {d['fp'][:12]}   "
                  f"{d['provenance'].get('at','?')[:19]}")
            for idx, name, real in wrong[:8]:
                print(f"      index {idx:3d}: map says {name!r}, the plugin reported {real}")
    if not bad0:
        print("\n  none")
    print(f"\n  {checked0} of {len(maps)} maps had live evidence to check against.")

    print("\n" + "=" * 74)
    print("CHECK 1 -- a map that contradicts itself about an index  (PROOF)")
    print("=" * 74)
    suspect, checkable, blind = [], 0, []
    for f, d in maps:
        bad, overlap = index_disagreements(d)
        if overlap:
            checkable += 1
        else:
            blind.append(d["identity"]["name"])
        if bad:
            suspect.append((f, d, bad))
            print(f"\n  {d['identity']['name']}   fp {d['fp'][:12]}   "
                  f"submitted {d['provenance'].get('at','?')[:19]}")
            for i, pname, cname in bad:
                print(f"      index {i:3d}:  params say {pname!r}   controls say {cname!r}")
    if not suspect:
        print("\n  none")
    print(f"\n  {checkable} of {len(maps)} maps had an index in BOTH tiers, so the check "
          f"could see them.\n  {len(blind)} could not be checked this way.")

    print("\n" + "=" * 74)
    print("CHECK 2 -- two DIFFERENT products with an identical control surface")
    print("=" * 74)

    # A map is CLEARED BY THE PROOF when check 0 covered every one of its
    # controls against the live record and none was wrong. A weaker heuristic
    # must not override a passing proof: UAD's 1176SE/1176LN Legacy and
    # Precision Delay Mod/Mod L are different products that GENUINELY share a
    # surface -- same names, ranges and anchor counts -- and check 0 verified
    # each map's controls against what its own plugin reported. Flagging those
    # as leaks would train the reader to dismiss this check.
    def cleared_by_proof(d):
        i = d["identity"]
        key = f"{i.get('format')}|{(i.get('uid') or '').lower()}|{i.get('version')}"
        seen = obs.get(ids.get(key) or "", {})
        controls = d.get("controls") or {}
        if not controls or not seen:
            return False
        for name, c in controls.items():
            idx = c.get("index")
            if idx is None or idx not in seen or name not in seen[idx]:
                return False
        return True

    by_sig = collections.defaultdict(list)
    for f, d in maps:
        if d.get("controls"):
            by_sig[control_signature(d)].append(d)
    flagged, twins = [], []
    for sig, ds in by_sig.items():
        products = {product_of(d) for d in ds}
        if len(ds) > 1 and len(products) > 1:
            if all(cleared_by_proof(d) for d in ds):
                twins.append(ds)
                continue
            flagged.append(ds)
            print(f"\n  {len(sig)} control(s), identical names AND ranges AND anchor counts:")
            for d in ds:
                print(f"      {d['identity']['name'][:38]:38s} fp {d['fp'][:12]} "
                      f"{d['provenance'].get('at','?')[:19]}")
    for ds in twins:
        print(f"\n  identical surface, CLEARED BY THE PROOF (every control of every "
              f"member verified against its own plugin's live record):")
        for d in ds:
            print(f"      {d['identity']['name'][:38]:38s} fp {d['fp'][:12]}  (true twin)")
    if not flagged:
        print("\n  none implicated")

    print("\n" + "=" * 74)
    print("CHECK 3 -- a control index past the plugin's parameter count  (PROOF)")
    print("=" * 74)
    any3 = False
    for f, d in maps:
        bad = out_of_range(d)
        if bad:
            any3 = True
            print(f"\n  {d['identity']['name']}  (param_count "
                  f"{d['identity'].get('param_count')})")
            for name, i, n in bad:
                print(f"      {name!r} at index {i}, plugin has {n}")
    if not any3:
        print("\n  none")

    print("\n" + "=" * 74)
    print("VERDICT")
    print("=" * 74)
    names = sorted({d["identity"]["name"] for _f, d, _b in suspect}
                   | {d["identity"]["name"] for ds in flagged for d in ds}
                   | {d["identity"]["name"] for d, _w in bad0})
    if names:
        print(f"\n  {len(names)} map(s) implicated:")
        for n in names:
            print(f"      {n}")
        print("\n  A map whose controls were measured on another plugin is NOT fixable by\n"
              "  editing: nothing in the control surface is a measurement of this plugin.\n"
              "  Withdraw it and re-map.")
    else:
        print("\n  No map contradicts itself and no two products share a surface.")
    print(f"\n  COVERAGE, stated rather than implied:")
    print(f"    check 0 saw {checked0} of {len(maps)} (needs live capture rows for that plugin)")
    print(f"    check 1 saw {checkable} of {len(maps)} (needs an index in BOTH tiers)")
    print(f"    check 2 sees every map, but only catches a leak whose DONOR is also mapped")
    unseen = [d["identity"]["name"] for f, d in maps
              if not index_disagreements(d)[1]
              and not obs.get(ids.get(f"{d['identity'].get('format')}|"
                                      f"{(d['identity'].get('uid') or '').lower()}|"
                                      f"{d['identity'].get('version')}", ""), {})]
    print(f"    {len(unseen)} map(s) are visible to check 2 alone")
    return 1 if names else 0


if __name__ == "__main__":
    sys.exit(main())
