#!/usr/bin/env python3
# wizard-deep-replay.py - replay the wizard fixtures against any server, field by field (B, EchoJay; for Kathy, 22 Oct 2026). Python 3 standard library only.
#
# HOW TO RUN
#   python3 -I wizard-deep-replay.py --base <server base URL> --fixtures <your fixtures folder> --shift-created [--json-out results.json]
#   e.g. against your mock:   python3 -I wizard-deep-replay.py --base http://127.0.0.1:8787 --fixtures fixtures --shift-created
#
# WHAT IT DOES (your replay.py, plus three things):
#   - sends each recorded request in seq order; binds every <ACCESS_TOKEN_n> / <REFRESH_TOKEN_n> / <DEVICE_CODE_n> / <USER_CODE_n> / <PROMO_CODE_n> to the live value
#     the server returns the first time; re-gzips results and recomputes X-EJ-Content-SHA256, except where the fixture says sha_header_matched: false;
#   - --shift-created moves every result's "created" so the earliest is a minute ago, KEEPING THE GAPS (agreed 22 Oct: a server that checks created against
#     the run's own time refuses a stamp from the recording day);
#   - compares field by field, not just the status: error / status / reason, the counts (results_received, results_accepted, plugins_checked, missing),
#     each plugin's catalogue answer (status, reason, level, category) and the promo (eligible, reason, issued_now, months, message; expires only for presence);
#   - sends the /__mock/* test hooks to the same base: so the base must answer them (your mock does; B's local server does with --mock-hooks).
#     Against a server without them, run your replay.py, which stops and asks you to do each step by hand.
# Prints one line per exchange (ok / DIFF with each difference under it) and a total. Exit 0 always; read the DIFF lines.
import argparse, glob, gzip, hashlib, json, os, re, sys, urllib.error, urllib.request, urllib.parse
from datetime import datetime, timezone, timedelta
PH = re.compile(r"<(ACCESS_TOKEN|REFRESH_TOKEN|DEVICE_CODE|USER_CODE|PROMO_CODE)_\d+>")
def fill(x, b):
    if isinstance(x, str): return PH.sub(lambda m: b.get(m.group(0), m.group(0)), x)
    if isinstance(x, dict): return {k: fill(v, b) for k, v in x.items()}
    if isinstance(x, list): return [fill(v, b) for v in x]
    return x
def bind(e, a, b):
    if isinstance(e, str) and PH.fullmatch(e) and isinstance(a, str): b.setdefault(e, a)
    elif isinstance(e, dict) and isinstance(a, dict):
        for k in e: bind(e[k], a.get(k), b)
    elif isinstance(e, list) and isinstance(a, list):
        for x, y in zip(e, a): bind(x, y, b)
def send(base, rq, b):
    url = base.rstrip("/") + rq["path"]; h = {k: fill(v, b) for k, v in rq.get("headers", {}).items() if k.lower() != "content-length"}; data = None
    if "form" in rq: data = urllib.parse.urlencode(fill(rq["form"], b)).encode(); h["Content-Type"] = "application/x-www-form-urlencoded"
    elif "json" in rq: data = json.dumps(fill(rq["json"], b)).encode(); h["Content-Type"] = "application/json"
    elif "gzip_json" in rq:
        plain = json.dumps(fill(rq["gzip_json"], b)).encode(); data = gzip.compress(plain); h["Content-Encoding"] = "gzip"; h["Content-Type"] = "application/json"
        if rq.get("sha_header_matched", True): h["X-EJ-Content-SHA256"] = hashlib.sha256(plain).hexdigest()
    req = urllib.request.Request(url, data=data, method=rq["method"], headers=h)
    try:
        with urllib.request.urlopen(req, timeout=30) as r: return r.status, json.loads(r.read().decode() or "null")
    except urllib.error.HTTPError as e:
        t = e.read().decode()
        try: return e.code, json.loads(t or "null")
        except ValueError: return e.code, t
def diffs(exp, got):
    out = []
    if not isinstance(exp, dict) or not isinstance(got, dict): return [] if exp == got else ["body %r vs %r" % (exp, got)]
    for k in ("error", "status", "reason", "created", "results_received", "results_accepted", "plugins_checked", "missing"):
        if k in exp and exp.get(k) != got.get(k): out.append("%s: expected %r, got %r" % (k, exp.get(k), got.get(k)))
    if isinstance(exp.get("promo"), dict):
        gp = got.get("promo") or {}
        for k in ("eligible", "reason", "issued_now", "months", "expires", "message"):
            if k in exp["promo"] and exp["promo"].get(k) != gp.get(k) and not (k == "expires"): out.append("promo.%s: expected %r, got %r" % (k, exp["promo"].get(k), gp.get(k)))
        if "expires" in exp["promo"] and "expires" not in gp: out.append("promo.expires missing")
    if isinstance(exp.get("results"), list):
        gr = {r.get("plugin_id"): r for r in (got.get("results") or [])}
        for e in exp["results"]:
            g = gr.get(e["plugin_id"], {})
            for k in ("status", "reason", "level", "category"):
                if e.get(k) != g.get(k): out.append("%s %s: expected %r, got %r" % (e["plugin_id"].split("|")[1], k, e.get(k), g.get(k)))
    if "message" in exp and exp.get("message") and exp.get("message") != got.get("message"): out.append("message: expected %r, got %r" % (exp["message"], got.get("message")))
    return out
def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--base", required=True); ap.add_argument("--fixtures", required=True); ap.add_argument("--shift-created", action="store_true"); ap.add_argument("--json-out")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.fixtures, "*", "*.json")), key=lambda f: json.load(open(f))["seq"])
    fx_all = [json.load(open(f)) for f in files]
    shift = timedelta(0)
    if a.shift_created:
        ts = [datetime.fromisoformat(fx["request"]["gzip_json"]["created"]) for fx in fx_all if isinstance(fx["request"].get("gzip_json"), dict) and fx["request"]["gzip_json"].get("created")]
        shift = (datetime.now(timezone.utc) - timedelta(minutes=1)) - min(ts)
    b, rows = {}, []
    for fx in fx_all:
        rq = json.loads(json.dumps(fx["request"]))
        if shift and isinstance(rq.get("gzip_json"), dict) and rq["gzip_json"].get("created"):
            rq["gzip_json"]["created"] = (datetime.fromisoformat(rq["gzip_json"]["created"]) + shift).isoformat(timespec="milliseconds")
        if fx.get("test_hook"):
            send(a.base, rq, b); continue
        st, body = send(a.base, rq, b); exp = fx["response"]; bind(exp["body"], body, b)
        d = ([] if st == exp["status"] else ["http: expected %s, got %s" % (exp["status"], st)]) + diffs(exp["body"], body)
        rows.append({"seq": fx["seq"], "scenario": fx["scenario"], "method": rq["method"], "path": rq["path"], "diffs": d, "got": body})
        print("%s %05d %-38s %-5s %s" % ("ok  " if not d else "DIFF", fx["seq"], fx["scenario"], rq["method"], rq["path"][:70]))
        for x in d: print("        - " + x)
    if a.json_out: json.dump(rows, open(a.json_out, "w"), indent=1)
    print("\ndeep replay: %d exchanges, %d with differences" % (len(rows), sum(1 for r in rows if r["diffs"])))
main()
