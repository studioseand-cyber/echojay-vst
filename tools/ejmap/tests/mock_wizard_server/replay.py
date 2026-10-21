#!/usr/bin/env python3
"""
REPLAY the wizard contract fixtures against any server (contract v1, FOR_SEAN_21oct/WIZARD_SERVER_CONTRACT_v1.md).

    python3 replay.py --base https://staging.example --fixtures fixtures [--scenario 03_duplicate_upload] [--mock] [--client-id ID]

Each fixture is one request the real EchoJay Wizard client made and the answer our mock gave (status + body). The replay sends the
same requests, in order, and compares the status (and the "error" / "status" / "reason" fields) with what the mock answered.

PLACEHOLDERS: tokens, device codes, user codes and promo codes are scrubbed to <ACCESS_TOKEN_1>, <REFRESH_TOKEN_2>, <DEVICE_CODE_1>,
<USER_CODE_1>, <PROMO_CODE_1> ... The replay binds each placeholder to the real value YOUR server returns the first time it appears in a
response, and puts that value back into the later requests that used it. Promo codes are only checked for presence.

TEST HOOKS: fixtures with "test_hook": true are our mock's /__mock/* calls (approve a sign-in, expire tokens, force a 429, set config).
Your server has no such endpoints:
  --mock     send them (only when --base is our mock_server.py);
  otherwise  the replay stops and tells you what to do by hand on your side (e.g. approve <USER_CODE_1> as test account B), then
             carries on when you press Return.
Results are re-gzipped and their X-EJ-Content-SHA256 recomputed, EXCEPT where the wizard deliberately sent a wrong one (the cheat-check
fixture: "sha_header_matched": false), which is sent wrong again.
"""
import argparse, glob, gzip, hashlib, json, os, re, sys, urllib.error, urllib.parse, urllib.request

PH = re.compile(r"<(ACCESS_TOKEN|REFRESH_TOKEN|DEVICE_CODE|USER_CODE|PROMO_CODE)_\d+>")
HOOK_WORDS = {
    "/__mock/approve": "approve the waiting sign-in in the browser as test account {account_id}",
    "/__mock/deny": "DENY the waiting sign-in in the browser",
    "/__mock/expire_access": "make the access tokens issued so far expire (or wait for them to)",
    "/__mock/config": "set your server's config to {body} for the next steps",
    "/__mock/catalogue": "make your catalogue hold {body}",
    "/__mock/verify": "verify {plugin_id} yourselves (our own verification)",
}


def fill(x, bound):
    if isinstance(x, str):
        return PH.sub(lambda m: bound.get(m.group(0), m.group(0)), x)
    if isinstance(x, dict):
        return {k: fill(v, bound) for k, v in x.items()}
    if isinstance(x, list):
        return [fill(v, bound) for v in x]
    return x


def bind(expected, actual, bound):
    """walk the recorded response next to the live one: a placeholder in the recording names the live value at the same place"""
    if isinstance(expected, str) and PH.fullmatch(expected) and isinstance(actual, str):
        bound.setdefault(expected, actual)
    elif isinstance(expected, dict) and isinstance(actual, dict):
        for k in expected:
            bind(expected[k], actual.get(k), bound)
    elif isinstance(expected, list) and isinstance(actual, list):
        for e, a in zip(expected, actual):
            bind(e, a, bound)


def send(base, rq, bound):
    url = base.rstrip("/") + rq["path"]
    headers = {k: fill(v, bound) for k, v in rq.get("headers", {}).items() if k.lower() != "content-length"}
    data = None
    if "form" in rq:
        data = urllib.parse.urlencode(fill(rq["form"], bound)).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    elif "json" in rq:
        data = json.dumps(fill(rq["json"], bound)).encode()
        headers["Content-Type"] = "application/json"
    elif "gzip_json" in rq:
        plain = json.dumps(fill(rq["gzip_json"], bound)).encode()
        data = gzip.compress(plain)
        headers["Content-Encoding"] = "gzip"
        headers["Content-Type"] = "application/json"
        if rq.get("sha_header_matched", True):
            headers["X-EJ-Content-SHA256"] = hashlib.sha256(plain).hexdigest()
    req = urllib.request.Request(url, data=data, method=rq["method"], headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read().decode() or "null")
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        try:
            return e.code, json.loads(body or "null")
        except ValueError:
            return e.code, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--fixtures", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures"))
    ap.add_argument("--scenario", action="append")
    ap.add_argument("--mock", action="store_true", help="the base is our mock: send the test hooks")
    ap.add_argument("--client-id", help="replace the mock's client id (ejwiz-mock) with yours")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.fixtures, "*", "*.json")), key=lambda f: json.load(open(f))["seq"])
    bound, ok, bad = {}, 0, 0
    for f in files:
        fx = json.load(open(f))
        if a.scenario and fx["scenario"] not in a.scenario:
            continue
        rq = fx["request"]
        if a.client_id:
            rq = json.loads(json.dumps(rq).replace("ejwiz-mock", a.client_id))
        if fx.get("test_hook"):
            if a.mock:
                send(a.base, rq, bound)
            else:
                body = fill(rq.get("json", {}), bound)
                words = HOOK_WORDS.get(rq["path"], "do " + rq["path"]).format(body=json.dumps(body), **{k: body.get(k, "?") for k in ("account_id", "plugin_id")})
                input("[%s] ON YOUR SERVER: %s - then press Return " % (fx["scenario"], fill(words, bound)))
            continue
        status, body = send(a.base, rq, bound)
        exp = fx["response"]
        bind(exp["body"], body, bound)
        same = status == exp["status"] and all((body or {}).get(k) == exp["body"].get(k) for k in ("error", "status", "reason") if isinstance(exp["body"], dict) and k in exp["body"] and isinstance(body, dict))
        if isinstance(exp["body"], dict) and isinstance(exp["body"].get("promo"), dict) and isinstance(body, dict):
            same = same and body.get("promo", {}).get("eligible") == exp["body"]["promo"].get("eligible") and body.get("promo", {}).get("reason") == exp["body"]["promo"].get("reason")
        ok += same
        bad += not same
        print("%s %-40s %-6s %-55s expected %s, got %s" % ("ok  " if same else "DIFF", fx["scenario"], rq["method"], rq["path"][:55], exp["status"], status))
    print("\nreplay: %d matched, %d differ" % (ok, bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
