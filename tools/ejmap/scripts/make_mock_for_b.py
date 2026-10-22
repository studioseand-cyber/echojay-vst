#!/usr/bin/env python3
"""
make_mock_for_b.py - MOCK_FOR_B.zip (Kathy, 21 Oct): the contract v1 mock, a README, scrubbed request/response fixtures captured from the
real wizard client, the replay script, and result-size stats from a real run. Nothing is sent anywhere.

  python3 make_mock_for_b.py --raw <recorded exchanges> --engine <a wizard run's engine folder> --out <zip> [--stage <folder>]

  --raw     the mock's --record folder from a run of the contract pins (EJ_CAPTURE_DIR=... ejmap-roundtrip-test)
  --engine  e.g. ~/Desktop/WIZARD_RUN_20oct_DONE/EchoJay Wizard Test/engine (read only: file sizes and nothing else leave it)

SCRUB: every token, device code, user code and promo code -> a numbered placeholder (<ACCESS_TOKEN_1> ...), the same value always the
same placeholder. The finished folder AND the zip are then scanned for token-, key-, email-, path- and promo-shaped strings; any hit
fails the build (exit 1) and no zip is left behind.
"""
import argparse, glob, gzip, hashlib, io, json, os, re, shutil, statistics, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOCK_DIR = os.path.join(HERE, "..", "tests", "mock_wizard_server")
KINDS = {"access_token": "ACCESS_TOKEN", "refresh_token": "REFRESH_TOKEN", "device_code": "DEVICE_CODE", "user_code": "USER_CODE"}


# --- 1. SCRUB ------------------------------------------------------------------------------------------------------------------------
def collect(x, found, parent_key=None, in_promo=False):
    if isinstance(x, dict):
        for k, v in x.items():
            if isinstance(v, str) and k in KINDS and v and not v.startswith("<"):
                found.append((KINDS[k], v))
            if isinstance(v, str) and k == "code" and in_promo and v:
                found.append(("PROMO_CODE", v))
            if isinstance(v, str) and k.lower() == "authorization" and v.startswith("Bearer "):
                found.append(("ACCESS_TOKEN", v[7:]))
            if isinstance(v, str) and k == "token":   # /auth/revoke's form field (a refresh token)
                found.append(("REFRESH_TOKEN", v))
            collect(v, found, k, in_promo or k == "promo")
    elif isinstance(x, list):
        for v in x:
            collect(v, found, parent_key, in_promo)


def replace_all(x, table):
    if isinstance(x, str):
        for secret, ph in table:
            x = x.replace(secret, ph)
        return x
    if isinstance(x, dict):
        return {replace_all(k, table): replace_all(v, table) for k, v in x.items()}
    if isinstance(x, list):
        return [replace_all(v, table) for v in x]
    return x


def scrub(raw_dir, out_dir):
    recs = [json.load(open(f)) for f in sorted(glob.glob(os.path.join(raw_dir, "*.json")))]
    recs.sort(key=lambda r: r["seq"])
    found = []
    for r in recs:
        collect(r, found)
    names, counters = {}, {}
    for kind, value in found:
        if value in names:
            continue
        counters[kind] = counters.get(kind, 0) + 1
        names[value] = "<%s_%d>" % (kind, counters[kind])
    table = sorted(names.items(), key=lambda kv: -len(kv[0]))   # longest first: no partial replacement
    for r in recs:
        r = replace_all(r, table)
        d = os.path.join(out_dir, "fixtures", r["scenario"])
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "%05d.json" % r["seq"]), "w") as f:
            json.dump(r, f, indent=1)
    return len(recs), counters


# --- 2. SIZES ------------------------------------------------------------------------------------------------------------------------
def kind_of(path):
    n = os.path.basename(path)
    if n.endswith(".tonecheck.json"):
        return "compressor tone check (*.tonecheck.json, no schema field)"
    if "/wizard/profiles/" in path or "/limitercomp/profiles/" in path:
        return "compressor profile (ej_comp_profile/1)"
    m = re.search(r"\.([a-z_]+)\.draft\.json$", n)
    if m:
        return {"eq_profile": "EQ draft (ej_eq_profile)", "saturation_profile": "saturation draft", "space_profile": "reverb / delay draft",
                "strip_profile": "channel strip draft", "deesser_block": "de-esser draft", "transient_profile": "transient draft",
                "gate_profile": "gate draft", "limiter_profile": "limiter draft", "tuner_profile": "tuner / pitch draft"}.get(m.group(1), m.group(1) + " draft")
    if n.endswith(".readback.json") and "/readback/bridge/" in path:
        return "VST3 bridge read-back record"
    if "/contribution/needs_category/" in path:
        return "needs_category (map + signature reads)"
    return None


def pct(sorted_vals, q):
    if not sorted_vals:
        return 0
    i = min(len(sorted_vals) - 1, max(0, int(round(q * (len(sorted_vals) - 1)))))
    return sorted_vals[i]


def sizes(engine):
    by = {}
    files = glob.glob(os.path.join(engine, "phaseb", "**", "*.json"), recursive=True) + glob.glob(os.path.join(engine, "contribution", "needs_category", "*.json"))
    pairs = {}
    for f in files:
        k = kind_of(f.replace(os.sep, "/"))
        if k is None:
            continue
        raw = open(f, "rb").read()
        gz = len(gzip.compress(raw, 9))
        by.setdefault(k, []).append((len(raw), gz))
        if k.startswith("compressor"):
            stem = os.path.basename(f).replace(".tonecheck.json", "").replace(".json", "")
            pairs.setdefault(stem, []).append(raw)
    comp = [b"".join(v) for v in pairs.values() if len(v) == 2]
    if comp:
        by["compressor result (profile + tone check together)"] = [(len(c), len(gzip.compress(c, 9))) for c in comp]
    out = {}
    for k, vals in sorted(by.items()):
        r = sorted(v[0] for v in vals); g = sorted(v[1] for v in vals)
        out[k] = {"count": len(vals),
                  "raw_bytes": {"min": r[0], "median": int(statistics.median(r)), "p95": pct(r, 0.95), "max": r[-1]},
                  "gzip_bytes": {"min": g[0], "median": int(statistics.median(g)), "p95": pct(g, 0.95), "max": g[-1]}}
    return out


# --- 3. THE SCAN: fail on anything token-, key-, email-, path- or promo-shaped -----------------------------------------------------------
SCAN = [
    ("mock access / refresh token", re.compile(r"\b(at|rt)_[A-Za-z0-9_-]{20,}")),
    ("bearer token", re.compile(r"Bearer (?!<ACCESS_TOKEN_\d+>)[A-Za-z0-9._~+/=-]{8,}")),
    ("token / code field with a real value", re.compile(r'"(access_token|refresh_token|device_code|user_code|token)"\s*:\s*"(?!<)[^"]{4,}"')),
    ("form token value", re.compile(r"(device_code|refresh_token|token)=(?!%3C|<)[A-Za-z0-9_-]{8,}")),
    ("promo code", re.compile(r"EJ-FREE-[0-9A-F]{4,}")),
    ("promo code field", re.compile(r'"code"\s*:\s*"(?!<)[^"]+"')),
    ("email address", re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")),
    ("user path", re.compile(r"/Users/|/home/|/private/(tmp|var)/")),
    ("private key", re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("cloud / payment / chat key", re.compile(r"sk_(live|test)_[0-9A-Za-z]{10,}|AKIA[0-9A-Z]{16}|gh[pousr]_[0-9A-Za-z]{30,}|xox[baprs]-[0-9A-Za-z-]{10,}")),
    ("jwt", re.compile(r"eyJ[0-9A-Za-z_-]{10,}\.eyJ[0-9A-Za-z_-]{10,}")),
    ("a person's name", re.compile(r"\b(Kathy|Sean|SeanD|Donoghue)\b")),
]
ALLOW_EMAIL = {"support@echojay.ai"}   # the product's public support address, in B's own account_blocked message (B's code list, 22 Oct)
# B's own file, shipped back unchanged: its header says who it was written for. Exempt from that one label, said in the build's output
ALLOW_LABEL_IN = {"wizard-deep-replay.py": {"a person's name"}}


def scan_text(name, text):
    hits = []
    for label, rx in SCAN:
        if label in ALLOW_LABEL_IN.get(os.path.basename(name), ()):
            continue
        for m in rx.finditer(text):
            if label == "email address" and m.group(0) in ALLOW_EMAIL:
                continue
            hits.append("%s: %s (%s...)" % (name, label, m.group(0)[:10]))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", required=True); ap.add_argument("--engine", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--stage")
    a = ap.parse_args()
    stage = a.stage or (a.out + ".stage")
    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    n, counters = scrub(a.raw, stage)
    for f in ("mock_server.py", "replay.py", "wizard-deep-replay.py"):
        shutil.copy(os.path.join(MOCK_DIR, f), os.path.join(stage, f))
    st = sizes(a.engine)
    json.dump(st, open(os.path.join(stage, "result_sizes.json"), "w"), indent=1)
    with open(os.path.join(stage, "result_sizes.md"), "w") as f:
        f.write("# Result sizes from a real wizard run (20 Oct 2026, one Mac)\n\nBytes per file, raw and gzipped (level 9, as the wizard sends). A result carries its type's file(s) plus a small envelope (~0.5 KB).\n\n")
        f.write("| result type | count | raw min / median / 95th / max | gzip min / median / 95th / max |\n|---|---|---|---|\n")
        for k, v in st.items():
            r, g = v["raw_bytes"], v["gzip_bytes"]
            f.write("| %s | %d | %d / %d / %d / %d | %d / %d / %d / %d |\n" % (k, v["count"], r["min"], r["median"], r["p95"], r["max"], g["min"], g["median"], g["p95"], g["max"]))
    shutil.copy(os.path.join(MOCK_DIR, "README_FOR_B.md"), os.path.join(stage, "README.md"))
    # THE SCAN, on the folder, then on the zip itself
    hits = []
    for p in glob.glob(os.path.join(stage, "**", "*"), recursive=True):
        if os.path.isfile(p):
            hits += scan_text(os.path.relpath(p, stage), open(p, errors="replace").read())
    if hits:
        print("REFUSED - the package would carry:\n  " + "\n  ".join(hits[:40]))
        sys.exit(1)
    if os.path.exists(a.out):
        os.remove(a.out)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(glob.glob(os.path.join(stage, "**", "*"), recursive=True)):
            if os.path.isfile(p):
                z.write(p, os.path.join("MOCK_FOR_B", os.path.relpath(p, stage)))
    with zipfile.ZipFile(a.out) as z:
        for info in z.infolist():
            hits += scan_text("zip:" + info.filename, z.read(info).decode(errors="replace"))
            hits += scan_text("zip name", info.filename)
    if hits:
        os.remove(a.out)
        print("REFUSED (in the zip) - removed:\n  " + "\n  ".join(hits[:40]))
        sys.exit(1)
    print("scan exemptions: %s" % "; ".join("%s: %s" % (f, ", ".join(sorted(l))) for f, l in ALLOW_LABEL_IN.items()))
    print("MOCK_FOR_B: %d fixture(s) in %d scenario(s); placeholders %s; %d result type(s) sized; scan clean -> %s"
          % (n, len(os.listdir(os.path.join(stage, "fixtures"))), counters, len(st), a.out))


if __name__ == "__main__":
    main()
