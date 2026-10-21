#!/usr/bin/env python3
"""
THE MOCK WIZARD SERVER - contract v1 (FOR_SEAN_21oct/WIZARD_SERVER_CONTRACT_v1.md), Python standard library only, localhost only.

The wizard's client (EjmapWizardContract.h) is proven against this in the tests (RoundTripTest WZ-NET-*). B can run it to see every
request and answer:  python3 mock_server.py --port 8787  (--port 0 picks a free port; --port-file F writes the chosen port to F).

What it does as the contract says: device sign-in (RFC 8628, public client, refresh rotation + reuse detection, revoke), the catalogue
check, runs (idempotent, one account), results (gzip, SHA-256, idempotent per result id, size limits, a stand-in validator, a per-user
HOLDING area - nothing is "served"), the run's end (the server's eligibility, one promo per account, the same code on a repeat).

The /__mock/* endpoints are TEST HOOKS ONLY (approve a sign-in, expire tokens, set the config, read the state) - never part of the
contract.
"""
import argparse, gzip, hashlib, json, os, secrets, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

MAX_COMPRESSED = 4_000_000
MAX_UNCOMPRESSED = 32 * 1024 * 1024
CATEGORIES = {"compressor", "limiter", "eq", "de-esser", "delay", "reverb", "saturation", "gate", "transient_shaper", "channel_strip", "amp_sim", "pitch"}
FINAL_STATES = {"measured", "not_measurable", "needs_licence", "needs_hardware", "crashed", "demo", "silent", "wont_load", "custom_default",
                "timed_out", "waves_skipped", "not_reached", "ready_from_version", "needs_category", "other_category", "out_of_scope", "held"}
PROFILE_SCHEMAS = {"ej_comp_profile/1", "ej_eq_profile/1", "ej_saturation_profile/1", "ej_tonecheck/1", "ej_limiter_profile/1",
                   "ej_deesser_profile/1", "ej_gate_profile/1", "ej_transient_profile/1", "ej_reverb_delay_profile/1", "ej_tuner_profile/1"}

LOCK = threading.RLock()   # re-entrant: a handler holding it also logs the request through _send
STATE = {
    "config": {"client_id": "ejwiz-mock", "qualify_minimum": 25, "access_ttl_s": 3600, "force_429_once": False,
               "collection_threshold": 0.95, "collection_min_size": 20, "agree_tolerance": 0.5},
    "devices": {},        # device_code -> {user_code, account_id, approved, denied, expires}
    "access": {},         # access token -> {account_id, expires, family}
    "refresh": {},        # refresh token -> {account_id, family, used}
    "dead_families": set(),
    "accounts": {"acct_mock_kathy": {"display_name": "Kathy"}, "acct_mock_other": {"display_name": "Other tester"}, "acct_mock_third": {"display_name": "Third tester"}, "acct_mock_fourth": {"display_name": "Fourth tester"}},
    "checked": {},        # account_id -> set of plugin_ids it sent to /catalogue/check (the server's own record; contract v1 §5)
    "catalogue": {},      # plugin_id -> {status, level, category, reason, fingerprint}
    "runs": {},           # run_id -> {account_id, consent, results: {result_id: sha}, completed: bool}
    "holding": {},        # account_id -> {result_id: body}    (NEVER served)
    "rejected": [],
    "promos": {},         # account_id -> code
    "served": {},         # plugin_id -> {level: mapped_unverified | certified, account, run, files, category, disagreed: n}   (contract v1 §4b)
    "requests": [],       # (method, path, status) - for the tests; never a body or a token
}


def now():
    return time.time()


def similarity(a, b):
    """contract v1 §5: |A ∩ B| / max(|A|, |B|) over exact plugin_ids"""
    a, b = set(a or []), set(b or [])
    return len(a & b) / max(len(a), len(b)) if a and b else 0.0


def near_identical(a, b):
    c = STATE["config"]
    return len(set(a or [])) >= c["collection_min_size"] and len(set(b or [])) >= c["collection_min_size"] and similarity(a, b) >= c["collection_threshold"]


def numbers(x, path=""):
    if isinstance(x, bool):
        return {}
    if isinstance(x, (int, float)):
        return {path: float(x)}
    out = {}
    if isinstance(x, dict):
        for k, v in x.items():
            out.update(numbers(v, path + "/" + k))
    elif isinstance(x, list):
        for i, v in enumerate(x):
            out.update(numbers(v, path + "/%d" % i))
    return out


def agree(files_a, files_b):
    """the stand-in for 'the same dial within the category's tolerance': every number both results carry within agree_tolerance"""
    na, nb = numbers(files_a), numbers(files_b)
    shared = set(na) & set(nb)
    return bool(shared) and all(abs(na[k] - nb[k]) <= STATE["config"]["agree_tolerance"] for k in shared)


def err(code, message, status, **extra):
    body = {"error": code, "message": message}
    body.update(extra)
    return status, body


class Handler(BaseHTTPRequestHandler):
    server_version = "EchoJayMockWizard/1"

    def log_message(self, *a):   # quiet: the tests read STATE["requests"]
        pass

    # --- plumbing -------------------------------------------------------------------------------------------------------------------
    def _send(self, status, body, headers=None):
        raw = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(raw)
        with LOCK:
            STATE["requests"].append([self.command, urlparse(self.path).path, status])

    def _raw_body(self, limit=MAX_COMPRESSED + 1):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(min(n, limit)) if n > 0 else b""

    def _form(self):
        return {k: v[0] for k, v in parse_qs(self._raw_body().decode()).items()}

    def _json(self):
        raw = self._raw_body()
        try:
            return json.loads(raw.decode() or "null")
        except ValueError:
            return None

    def _account(self):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            return None, err("token_revoked", "Please sign in again - your results are kept.", 401)
        tok = auth[7:]
        with LOCK:
            a = STATE["access"].get(tok)
            if a is None or a["family"] in STATE["dead_families"]:
                return None, err("token_revoked", "You've been signed out. Please sign in again - your results are kept.", 401)
            if a["expires"] < now():
                return None, err("token_expired", "", 401)
            return a["account_id"], None

    def _issue(self, account_id, family=None):
        family = family or secrets.token_hex(8)
        access, refresh = "at_" + secrets.token_urlsafe(24), "rt_" + secrets.token_urlsafe(32)
        STATE["access"][access] = {"account_id": account_id, "expires": now() + STATE["config"]["access_ttl_s"], "family": family}
        STATE["refresh"][refresh] = {"account_id": account_id, "family": family, "used": False}
        return {"access_token": access, "token_type": "Bearer", "expires_in": STATE["config"]["access_ttl_s"], "refresh_token": refresh,
                "scope": "wizard", "account": {"account_id": account_id, "display_name": STATE["accounts"][account_id]["display_name"]}}

    # --- routes ---------------------------------------------------------------------------------------------------------------------
    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/__mock/state":
            with LOCK:
                s = {k: v for k, v in STATE.items() if k not in ("access", "refresh", "devices", "dead_families", "checked")}
                s["checked_counts"] = {a: len(c) for a, c in STATE["checked"].items()}
                s["holding_counts"] = {a: len(r) for a, r in STATE["holding"].items()}
                s["served"] = {pid: {k: v for k, v in sv.items() if k != "files"} for pid, sv in STATE["served"].items()}
                s.pop("holding")
            return self._send(200, s)
        if path == "/api/wizard/v1/me":
            acct, e = self._account()
            if e:
                return self._send(*e)
            return self._send(200, {"account_id": acct, "display_name": STATE["accounts"][acct]["display_name"], "promo": {"issued": acct in STATE["promos"]}})
        return self._send(*err("not_found", "", 404))

    def do_POST(self):
        path = urlparse(self.path).path
        if path.startswith("/__mock/"):
            return self._hook(path, self._json() or {})
        if path == "/api/wizard/v1/auth/device":
            f = self._form()
            if f.get("client_id") != STATE["config"]["client_id"]:
                return self._send(*err("invalid_client", "This wizard isn't set up for this server.", 400))
            dc, uc = secrets.token_urlsafe(32), secrets.token_hex(2).upper() + "-" + secrets.token_hex(2).upper()
            with LOCK:
                STATE["devices"][dc] = {"user_code": uc, "account_id": None, "approved": False, "denied": False, "expires": now() + 600, "last_poll": 0}
            return self._send(200, {"device_code": dc, "user_code": uc, "verification_uri": "http://127.0.0.1/activate",
                                    "verification_uri_complete": "http://127.0.0.1/activate?code=" + uc, "expires_in": 600, "interval": 1})
        if path == "/api/wizard/v1/auth/token":
            return self._token(self._form())
        if path == "/api/wizard/v1/auth/revoke":
            f = self._form()
            with LOCK:
                r = STATE["refresh"].get(f.get("token", "")) or STATE["access"].get(f.get("token", ""))
                if r:
                    STATE["dead_families"].add(r["family"])
            return self._send(200, {})
        acct, e = self._account()
        if e:
            return self._send(*e)
        if STATE["config"].get("force_429_once"):
            STATE["config"]["force_429_once"] = False
            return self._send(*err("rate_limited", "EchoJay is busy - the wizard will carry on sending in a moment.", 429, retry_after_s=1), {"Retry-After": "1"})
        if path == "/api/wizard/v1/catalogue/check":
            return self._catalogue(self._json() or {})
        if path == "/api/wizard/v1/runs":
            return self._new_run(acct, self._json() or {})
        if path.startswith("/api/wizard/v1/runs/") and path.endswith("/complete"):
            return self._complete(acct, path.split("/")[5], self._json() or {})
        return self._send(*err("not_found", "", 404))

    def do_PUT(self):
        path = urlparse(self.path).path
        acct, e = self._account()
        if e:
            return self._send(*e)
        if STATE["config"].get("force_429_once"):
            STATE["config"]["force_429_once"] = False
            return self._send(*err("rate_limited", "EchoJay is busy - the wizard will carry on sending in a moment.", 429, retry_after_s=1), {"Retry-After": "1"})
        parts = path.split("/")   # ['', 'api', 'wizard', 'v1', 'runs', run, 'results', result]
        if len(parts) == 8 and parts[4] == "runs" and parts[6] == "results":
            return self._result(acct, parts[5], parts[7])
        return self._send(*err("not_found", "", 404))

    # --- the contract's calls -------------------------------------------------------------------------------------------------------
    def _token(self, f):
        if f.get("client_id") != STATE["config"]["client_id"]:
            return self._send(*err("invalid_client", "", 400))
        with LOCK:
            if f.get("grant_type") == "urn:ietf:params:oauth:grant-type:device_code":
                d = STATE["devices"].get(f.get("device_code", ""))
                if d is None or d["expires"] < now():
                    return self._send(*err("expired_token", "That sign-in link has expired - try again.", 400))
                if d["denied"]:
                    return self._send(*err("access_denied", "Sign-in was cancelled.", 400))
                if not d["approved"]:
                    return self._send(*err("authorization_pending", "", 400))
                STATE["devices"].pop(f["device_code"])
                return self._send(200, self._issue(d["account_id"]))
            if f.get("grant_type") == "refresh_token":
                r = STATE["refresh"].get(f.get("refresh_token", ""))
                if r is None or r["family"] in STATE["dead_families"]:
                    return self._send(*err("invalid_grant", "Please sign in again - your results are kept.", 400))
                if r["used"]:   # reuse of a rotated token: the whole family dies (theft detection)
                    STATE["dead_families"].add(r["family"])
                    return self._send(*err("invalid_grant", "Please sign in again - your results are kept.", 400))
                r["used"] = True
                return self._send(200, self._issue(r["account_id"], r["family"]))
        return self._send(*err("unsupported_grant_type", "", 400))

    def _catalogue(self, body):
        plugins = body.get("plugins") or []
        if len(plugins) > 2000:
            return self._send(*err("too_many", "", 413))
        out = []
        acct = self._account()[0]
        with LOCK:
            STATE["checked"].setdefault(acct, set()).update(p.get("plugin_id", "") for p in plugins)
        for p in plugins:
            pid = p.get("plugin_id", "")
            sv = STATE["served"].get(pid)
            if sv is not None and pid not in STATE["catalogue"]:   # a tester's result, at its level (contract v1 §4b)
                if sv["level"] == "certified":
                    out.append({"plugin_id": pid, "status": "have", "level": "certified", "category": sv.get("category")})
                elif sv["account"] == self._account()[0]:
                    out.append({"plugin_id": pid, "status": "have", "level": "mapped_unverified", "category": sv.get("category")})
                else:
                    out.append({"plugin_id": pid, "status": "need", "reason": "verify", "category": sv.get("category")})
                continue
            c = STATE["catalogue"].get(pid)
            if c is None:
                out.append({"plugin_id": pid, "status": "need", "category": None})
            elif c["status"] == "have" and p.get("fingerprint") and c.get("fingerprint") and p["fingerprint"] != c["fingerprint"]:
                out.append({"plugin_id": pid, "status": "need", "category": c.get("category"), "reason": "fingerprint_differs"})
            else:
                e = {"plugin_id": pid, "status": c["status"]}
                for k in ("level", "category", "reason"):
                    if c.get(k) is not None:
                        e[k] = c[k]
                out.append(e)
        return self._send(200, {"results": out, "categories_version": "mock-2026-10-21"})

    def _new_run(self, acct, body):
        rid = body.get("run_id", "")
        if not rid.startswith("run_") or len(rid) != 20:
            return self._send(*err("bad_request", "", 400))
        with LOCK:
            r = STATE["runs"].get(rid)
            if r is not None:
                if r["account_id"] != acct:
                    return self._send(*err("run_conflict", "This run belongs to another account. Sign in with the account you started it with.", 409))
                return self._send(200, {"run_id": rid, "created": False})
            STATE["runs"][rid] = {"account_id": acct, "consent": body.get("consent"), "client": body.get("client"), "results": {}, "accepted_profiles": 0, "completed": False,
                                  "collection": list(body.get("collection") or [])}
        return self._send(201, {"run_id": rid, "created": True})

    def _result(self, acct, rid, result_id):
        raw = self._raw_body()
        if len(raw) > MAX_COMPRESSED:
            return self._send(*err("too_large", "One result was too big to send and wasn't used.", 413))
        with LOCK:
            run = STATE["runs"].get(rid)
        if run is None:
            return self._send(*err("run_not_found", "", 404))
        if run["account_id"] != acct:
            return self._send(*err("run_conflict", "This run belongs to another account.", 409))
        try:   # stop decompressing at the limit: no zip bombs
            with gzip.GzipFile(fileobj=__import__("io").BytesIO(raw)) as g:
                plain = g.read(MAX_UNCOMPRESSED + 1)
        except OSError:
            return self._send(*err("rejected", "A result arrived damaged and wasn't used.", 422, reason="bad_gzip"))
        if len(plain) > MAX_UNCOMPRESSED:
            return self._send(*err("too_large", "One result was too big to send and wasn't used.", 413))
        sha = hashlib.sha256(plain).hexdigest()
        if sha != self.headers.get("X-EJ-Content-SHA256", ""):
            run["tampered"] = run.get("tampered", 0) + 1
            return self._send(*err("rejected", "A result arrived damaged and wasn't used.", 422, reason="sha_mismatch"))
        with LOCK:
            seen = run["results"].get(result_id)
            if seen is not None:
                if seen == sha:
                    return self._send(200, {"status": "duplicate", "result_id": result_id})
                return self._send(*err("result_conflict", "", 409))
        try:
            doc = json.loads(plain.decode())
        except ValueError:
            return self._send(*err("rejected", "A result arrived damaged and wasn't used.", 422, reason="not_json"))
        bad = validate(doc, rid, result_id)
        if bad:
            with LOCK:
                STATE["rejected"].append({"result_id": result_id, "reason": bad[0]})
                if bad[0] in ("bad_envelope", "bad_result_id", "identity_mismatch"):   # the cheat checks (contract v1 §5)
                    run["tampered"] = run.get("tampered", 0) + 1
            return self._send(*err("rejected", bad[1], 422, reason=bad[0]))
        with LOCK:
            run["results"][result_id] = sha
            if doc["kind"] == "profile" and doc["plugin"]["plugin_id"] not in STATE["catalogue"]:
                run["accepted_profiles"] += 1
                promote(doc, acct, rid, run)
            STATE["holding"].setdefault(acct, {})[result_id] = doc   # the HOLDING area - never served
        return self._send(201, {"status": "accepted", "result_id": result_id, "stored": "holding"})

    def _complete(self, acct, rid, body):
        with LOCK:
            run = STATE["runs"].get(rid)
            if run is None:
                return self._send(*err("run_not_found", "", 404))
            if run["account_id"] != acct:
                return self._send(*err("run_conflict", "This run belongs to another account.", 409))
            missing = [r for r in body.get("result_ids") or [] if r not in run["results"]]
            if missing:
                return self._send(*err("results_missing", "", 409, missing=missing))
            run["completed"] = True
            outcomes = body.get("outcomes") or []
            all_final = all(o.get("state") in FINAL_STATES for o in outcomes) and len(outcomes) >= len(body.get("queue") or [])
            accepted = run["accepted_profiles"]
            checked = len(set(run.get("collection") or []) & STATE["checked"].get(acct, set()))
            minimum = STATE["config"]["qualify_minimum"]
            res = {"status": "complete", "results_received": len(run["results"]), "results_accepted": accepted, "plugins_checked": checked}
            claimed_setup = any(r is not run and r.get("promo_code") and r["account_id"] != acct and near_identical(r.get("collection"), run.get("collection"))
                                for r in STATE["runs"].values())
            if acct in STATE["promos"]:
                code = STATE["promos"][acct]
                if run.get("promo_code") == code:   # the same run asking again: the same code, not issued now
                    res["promo"] = {"eligible": True, "code": code, "months": 1, "issued_now": False, "expires": "2027-01-31", "message": "Thank you - here's your free month."}
                else:
                    res["promo"] = {"eligible": False, "reason": "account_already_claimed", "message": "Your account already has its free month."}
            elif claimed_setup:   # contract v1 §5: once per plugin collection - near-identical to one that already earned a code
                res["promo"] = {"eligible": False, "reason": "collection_already_claimed", "message": "The free month has already been claimed for this setup."}
            elif body.get("kind") != "finished" or not all_final:
                res["promo"] = {"eligible": False, "reason": "not_finished", "message": "The free month needs a finished run - this one stopped early."}
            elif run.get("tampered", 0) > 0:
                res["promo"] = {"eligible": False, "reason": "failed_checks", "message": "Some results didn't pass EchoJay's checks, so the free month can't be given for this run."}
            elif checked < minimum:   # contract v1 §5 (Sean, 21 Oct): plugins CHECKED in the run, from the server's own records - not new uploads
                res["promo"] = {"eligible": False, "reason": "below_minimum", "message": "The free month needs at least %d plugins checked in a finished run; this run checked %d." % (minimum, checked)}
            else:
                code = "EJ-FREE-" + secrets.token_hex(4).upper()
                STATE["promos"][acct] = code
                run["promo_code"] = code
                res["promo"] = {"eligible": True, "code": code, "months": 1, "issued_now": True, "expires": "2027-01-31", "message": "Thank you - here's your free month."}
            return self._send(200, res)

    # --- test hooks (never part of the contract) ------------------------------------------------------------------------------------
    def _hook(self, path, body):
        with LOCK:
            if path == "/__mock/approve":   # the browser step: approve the waiting sign-in(s) for an account
                for d in STATE["devices"].values():
                    if not d["approved"] and not d["denied"]:
                        d["approved"], d["account_id"] = True, body.get("account_id", "acct_mock_kathy")
                return self._send(200, {})
            if path == "/__mock/deny":
                for d in STATE["devices"].values():
                    d["denied"] = True
                return self._send(200, {})
            if path == "/__mock/expire_access":
                for a in STATE["access"].values():
                    a["expires"] = 0
                return self._send(200, {})
            if path == "/__mock/config":
                STATE["config"].update(body)
                return self._send(200, STATE["config"])
            if path == "/__mock/catalogue":
                STATE["catalogue"].update(body)
                return self._send(200, {})
            if path == "/__mock/verify":   # our own verification certifies (contract v1 §4b)
                sv = STATE["served"].get(body.get("plugin_id", ""))
                if sv:
                    sv["level"], sv["certified_by"] = "certified", "echojay_verification"
                return self._send(200, {"level": sv["level"] if sv else None})
            if path == "/__mock/tokens_known":   # the tests ask whether a string is a live token, never the reverse
                return self._send(200, {"known": body.get("token") in STATE["access"] or body.get("token") in STATE["refresh"]})
        return self._send(*err("not_found", "", 404))


def promote(doc, acct, rid, run):
    """contract v1 §4b: the first passing result is served mapped_unverified; an INDEPENDENT second tester (another account, a collection
    not near-identical to the first's, another run) who agrees within tolerance makes it certified; one who disagrees leaves it for review"""
    pid = doc["plugin"]["plugin_id"]
    sv = STATE["served"].get(pid)
    if sv is None:
        STATE["served"][pid] = {"level": "mapped_unverified", "account": acct, "run": rid, "files": doc.get("files"),
                                "category": (doc.get("payload") or {}).get("category"), "disagreed": 0, "not_independent": 0}
        return
    if sv["level"] != "mapped_unverified":
        return
    first_run = STATE["runs"].get(sv["run"], {})
    if acct == sv["account"] or rid == sv["run"] or near_identical(first_run.get("collection"), run.get("collection")):
        sv["not_independent"] += 1
        return
    if agree(sv["files"], doc.get("files")):
        sv["level"], sv["certified_by"] = "certified", "second_tester"
    else:
        sv["disagreed"] += 1


def validate(doc, rid, result_id):
    """The stand-in for the served intake's validators (compProfile.js and its siblings): the result's shape, its id, and each file's
    schema and plugin identity. Returns (reason, plain words) or None."""
    if doc.get("schema") != "ej_wizard_result/1" or doc.get("run_id") != rid or doc.get("result_id") != result_id:
        return ("bad_envelope", "A result arrived damaged and wasn't used.")
    plugin = doc.get("plugin") or {}
    pid, kind = plugin.get("plugin_id", ""), doc.get("kind", "")
    if not pid or not plugin.get("name"):
        return ("no_identity", "A result had no plugin name and wasn't used.")
    expect = "res_" + hashlib.sha256(("%s|%s|%s" % (rid, kind, pid)).encode()).hexdigest()[:32]
    if expect != result_id:
        return ("bad_result_id", "A result arrived damaged and wasn't used.")
    if kind == "profile":
        files = doc.get("files") or {}
        if not files:
            return ("no_profile", "%s's measurements were missing, so they weren't used." % plugin["name"])
        for name, f in files.items():
            # a tone check travels as EJ Map writes it: no "schema" field, known by its name and its shape (seen in the first real run)
            if isinstance(f, dict) and name.endswith(".tonecheck.json") and "schema" not in f and "g_db" in f:
                continue
            if not isinstance(f, dict) or f.get("schema") not in PROFILE_SCHEMAS:
                return ("unknown_schema", "%s's measurements didn't pass our checks, so they weren't used." % plugin["name"])
            fp = (f.get("plugin") or {}).get("plugin_id")
            if fp is not None and fp != pid:
                return ("identity_mismatch", "%s's measurements didn't pass our checks, so they weren't used." % plugin["name"])
        cat = (doc.get("payload") or {}).get("category")
        if cat is not None and cat not in CATEGORIES:
            return ("unknown_category", "%s's measurements didn't pass our checks, so they weren't used." % plugin["name"])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--port-file")
    a = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)   # localhost only
    if a.port_file:
        tmp = a.port_file + ".tmp"
        with open(tmp, "w") as f:
            f.write(str(srv.server_address[1]))
        os.replace(tmp, a.port_file)
    try:   # a parent that closed our stdout (a test harness) must not stop the server
        print("mock wizard server on http://127.0.0.1:%d" % srv.server_address[1], flush=True)
    except (OSError, ValueError):
        pass
    srv.serve_forever()


if __name__ == "__main__":
    main()
