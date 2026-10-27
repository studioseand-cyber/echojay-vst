#!/usr/bin/env python3
"""
THE MOCK WIZARD SERVER - contract v1.1 (FOR_SEAN_22oct/WIZARD_SERVER_CONTRACT_v1.1.md; B's REASON_CODES_FOR_KATHY.md, 22 Oct), Python standard library only, localhost only.

The wizard's client (EjmapWizardContract.h) is proven against this in the tests (RoundTripTest WZ-NET-*). B can run it to see every
request and answer:  python3 mock_server.py --port 8787  (--port 0 picks a free port; --port-file F writes the chosen port to F).

What it does as the contract says: device sign-in (RFC 8628, public client, refresh rotation + reuse detection, revoke), the catalogue
check, runs (idempotent, one account), results (gzip, SHA-256, idempotent per result id, size limits, a stand-in validator, a per-user
HOLDING area - nothing is "served"), the run's end (the server's eligibility, one promo per account, the same code on a repeat).

The /__mock/* endpoints are TEST HOOKS ONLY (approve a sign-in, expire tokens, set the config, read the state) - never part of the
contract.
"""
import argparse, gzip, hashlib, json, os, re, secrets, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

MAX_COMPRESSED = 4_000_000
MAX_UNCOMPRESSED = 32 * 1024 * 1024
CATEGORIES = {"compressor", "limiter", "eq", "de-esser", "delay", "reverb", "saturation", "gate", "transient_shaper", "channel_strip", "amp_sim", "pitch", "filter"}   # v1.1: filter
FINAL_STATES = {"measured", "not_measurable", "needs_licence", "needs_hardware", "crashed", "demo", "silent", "wont_load", "custom_default",
                "timed_out", "waves_skipped", "not_reached", "ready_from_version", "needs_category", "other_category", "out_of_scope", "held",
                "bridged", "bridge_no_match"}   # 26 Oct
# v1.1: EXACTLY the schemas the wizard's drafts carry (the cert's draft writers; pinned by WZ-SCHEMAS-1) - the hand-written names before
# (ej_reverb_delay_profile/1, ej_deesser_profile/1, ej_limiter_profile/1) were never sent, so real reverb / delay / strip / de-esser / limiter
# results would have been refused (v1) or held (v1.1)
PROFILE_SCHEMAS = {"ej_comp_profile/1", "ej_eq_profile/1", "ej_saturation_profile/1", "ej_space_profile/1", "ej_strip_profile/0",
                   "ej_transient_profile/1", "ej_gate_profile/1", "ej_tuner_profile/1", "ej_filter_profile/1", "ej_pitch_profile/1",
                   "ej_limiter_profile/1", "ej_deesser_profile/1"}   # 24 Oct: ids, were sentences; known, and HELD (HELD_SCHEMAS) until B's validators exist
# B's answers (24 Oct): schemas B's server HOLDS by design until B has their validators - any result kind, never refused for them
HELD_SCHEMAS = {"ej_readback_sweep/1"}   # 27 Oct (B): only the bridge read's record stays held until B's validator exists
# 27 Oct (B): the two drafts B now VALIDATES - accepted (201) and kept as held_draft (never promoted or served yet); a draft missing its
# own block is refused as validator
DRAFT_SCHEMAS = {
    "ej_limiter_profile/1": lambda f: None if isinstance(f.get("ceiling"), dict) and (f["ceiling"].get("control") or f["ceiling"].get("by")) else "no ceiling block",
    "ej_deesser_profile/1": lambda f: None if isinstance(f.get("deesser"), dict) else "no deesser block",
}
MAP_SCHEMAS = {"2.4", "2.3"}   # 22 Oct (decision 0): a result's map.json is a FULL map in the map store's format


def map_problem(m, pid):
    """a map.json the server can use: the store's schema, at least one control, and the result's own plugin (uid in its plugin_id)"""
    if not isinstance(m, dict) or m.get("schema") not in MAP_SCHEMAS:
        return "not a map in the store's format"
    if not isinstance(m.get("controls"), dict) or not m["controls"]:
        return "a map with no controls"
    uid = str((m.get("identity") or {}).get("uid", "")).lower()
    if not uid or ("|" + uid + "|") not in ("|" + pid.lower() + "|"):
        return "a map for another plugin"
    return None

ALLOW_S = 300   # v1.1 §4: the timestamp allowance, 5 minutes either way
# v1.1 (24 Oct, agreed with B): measure_started_at is REQUIRED. A profile's measuring took at least its category's minimum (half the shortest
# real measuring time per category, 19-23 Oct, rounded down: MEASURE_TIMES_FOR_B.md); every other kind measured nothing for the profile and
# carries measure_started_at == measured_at - no minimum. No more than MAX_OVERLAP measurements of one run overlap in time (the wizard
# measures 4 at a time); a span of no length overlaps nothing
MIN_MEASURE_S = {"compressor": 76, "limiter": 76, "eq": 4, "saturation": 2, "de-esser": 6, "reverb": 6, "delay": 9, "filter": 1, "gate": 11,
                 "transient_shaper": 6, "channel_strip": 4, "pitch": 3, "amp_sim": 2}   # amp_sim: measured by the saturation pass
MAX_OVERLAP = 4


def overlap_with(spans, s, e):
    """the most measurements running at once in [s, e) - spans already accepted for the run, plus this one"""
    ev = [(s, 1), (e, -1)]
    for a, b in spans:
        if a < e and s < b:
            ev += [(max(a, s), 1), (min(b, e), -1)]
    ev.sort(key=lambda x: (x[0], x[1]))   # an end before a start at the same instant: back to back is not overlapping
    c = most = 0
    for _, k in ev:
        c += k
        most = max(most, c)
    return most
# B's codes (REASON_CODES_FOR_KATHY.md, 22 Oct): the 422 reasons and their words; <Plugin> is the result's plugin name
REJECT_WORDS = {
    "hash_mismatch": "One result arrived damaged, so it wasn't used.",
    "shape": "{p}'s measurements didn't pass our checks, so they weren't used.",
    "result_id": "{p}'s results didn't match how they were labelled, so they weren't used.",
    "identity": "{p}'s results don't match the plugin version they say they're from, so they weren't used.",
    "validator": "{p}'s measurements didn't pass our checks, so they weren't used.",
    "timestamp": "{p}'s results had an impossible time on them, so they weren't used.",
    "personal_detail": "{p}'s results included a detail about your computer, so they weren't used. Please update the EchoJay Wizard.",
}
TAMPERING = {"hash_mismatch", "result_id", "identity"}   # these also cost the run its free month (failed_checks)
WORDS = {
    "token_expired": "Your sign-in needs refreshing.",
    "token_revoked": "You've been signed out. Please sign in again - your results are kept.",
    "invalid_grant": "Please sign in again - your results are kept.",
    "invalid_client": "This version of the EchoJay Wizard isn't recognised. Please update it.",
    "rate_limited": "EchoJay is busy - the wizard will carry on sending in a moment.",
    "run_not_found": "That run isn't on the server yet.",
    "run_conflict": "This run belongs to another account. Sign in with the account you started it with.",
    "result_conflict": "That result was already sent with different contents.",
    "results_missing": "Some results still need sending.",
    "too_large": "One result was too big to send and wasn't used.",
    "account_blocked": "This account can't send results. Contact support@echojay.ai.",
    "client_too_old": "Please update the EchoJay Wizard - this version can no longer send results.",
    "run_id_required": "Please update the EchoJay Wizard - this version can't check your plugins.",
    "bad_request": "Something in that request wasn't right. Please update the EchoJay Wizard and try again.",
    "not_found": "Not found.",
    "server_error": "Something went wrong on our side. The wizard will try again.",
    "expired_token": "That sign-in link has expired - try again.",
    "access_denied": "Sign-in was cancelled.",
}


def parse_time(t):
    """an ISO 8601 time -> epoch seconds, or None"""
    from datetime import datetime
    if not isinstance(t, str) or not t:
        return None
    try:
        return datetime.fromisoformat(t.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def personal_detail(text):
    """v1.1 (B): a home folder, a user name, a computer name or an email field anywhere in the result"""
    import re
    return bool(re.search(r"/(Users|home)/|\"(email|user_name|computer_name|hostname)\"\s*:", text))


LOCK = threading.RLock()
RECORD = {"dir": None, "scenario": "unlabelled", "seq": 0}   # --record DIR: every exchange written raw (scrub before sharing: tools/ejmap/scripts/make_mock_for_b.py)   # re-entrant: a handler holding it also logs the request through _send
STATE = {
    "config": {"client_id": "ejwiz-mock", "qualify_minimum": 25, "access_ttl_s": 3600, "force_429_once": False,
               "collection_threshold": 0.95, "collection_min_size": 20, "agree_tolerance": 0.5},
    "devices": {},        # device_code -> {user_code, account_id, approved, denied, expires}
    "access": {},         # access token -> {account_id, expires, family}
    "refresh": {},        # refresh token -> {account_id, family, used}
    "dead_families": set(),
    "accounts": {"acct_mock_a": {"display_name": "Tester A"}, "acct_mock_b": {"display_name": "Tester B"}, "acct_mock_c": {"display_name": "Tester C"}, "acct_mock_d": {"display_name": "Tester D"},
                 "acct_mock_e": {"display_name": "Tester E"}, "acct_mock_f": {"display_name": "Tester F"},
                 "acct_mock_g": {"display_name": "Tester G"}},
    "catalogue_calls": [],   # 27 Oct: every check call - {run_id, n, delay_s} - so a test sees the chunks and the retries
    "checked": {},        # (account_id, run_id) -> set of plugin_ids checked under that run (the server's own record; contract v1 §2 / §5)
    "catalogue": {},      # plugin_id -> {status, level, category, reason, fingerprint}
    "runs": {},           # run_id -> {account_id, consent, results: {result_id: sha}, completed: bool}
    "holding": {},        # account_id -> {result_id: body}    (NEVER served)
    "rejected": [],
    "nonces": {},         # 24 Oct: a measurement's nonce -> the result_id it was first accepted under (a nonce is never reused)
    "promos": {},         # account_id -> code
    "served": {},         # plugin_id -> {level: mapped_unverified | certified, account, run, files, category, disagreed: n}   (contract v1 §4b)
    "requests": [],       # (method, path, status) - for the tests; never a body or a token
    "needs": {},          # (account_id, run_id) -> plugin_ids the check answered "need" for under that run (v1.1 §5: the server's queue)
    "first_check": {},    # (account_id, run_id) -> the time of the account's first catalogue check for the run (v1.1 §4 timestamps)
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


def comp_profile(files):
    for name, f in (files or {}).items():
        if isinstance(f, dict) and f.get("schema") == "ej_comp_profile/1":
            return f
    return None


def comp_agree(a, b):
    """v1.1 §4b (ruling 10): B's reading - the same position (amount norm) and GR step, the input level at most agree_tolerance dB apart,
    the same amount control, at least 3 shared points - PLUS the same plugin version and format, the same ratio setting and the same
    level convention (sine RMS)"""
    tol = STATE["config"]["agree_tolerance"]
    if (a.get("plugin") or {}).get("plugin_id") != (b.get("plugin") or {}).get("plugin_id"):   # format|uid|version
        return False
    if (a.get("measured") or {}).get("level_ref") != (b.get("measured") or {}).get("level_ref") or (a.get("measured") or {}).get("level_ref") != "sine_rms_dbfs":
        return False
    ra, rb = (a.get("ratio") or {}), (b.get("ratio") or {})
    sa = [(c.get("set"), c.get("norm")) for c in ra.get("curve") or []]
    sb = [(c.get("set"), c.get("norm")) for c in rb.get("curve") or []]
    if sa != sb or ra.get("control") != rb.get("control") or ra.get("fixed") != rb.get("fixed"):
        return False
    am, bm = a.get("amount") or {}, b.get("amount") or {}
    if am.get("control") != bm.get("control"):
        return False
    pa = {(round(float(c.get("norm", -1)), 4), k): v for c in am.get("curve") or [] for k, v in (c.get("in_at_gr_dbfs") or {}).items() if v is not None}
    pb = {(round(float(c.get("norm", -1)), 4), k): v for c in bm.get("curve") or [] for k, v in (c.get("in_at_gr_dbfs") or {}).items() if v is not None}
    shared = set(pa) & set(pb)
    return len(shared) >= 3 and all(abs(pa[k] - pb[k]) <= tol for k in shared)


def agree(files_a, files_b):
    """the same dial within the category's tolerance: a compressor by comp_agree (ruling 10); the other kinds: every number both results
    carry within agree_tolerance (the stand-in for their acceptance tolerances)"""
    ca, cb = comp_profile(files_a), comp_profile(files_b)
    if ca is not None or cb is not None:
        return ca is not None and cb is not None and comp_agree(ca, cb)
    na, nb = numbers(files_a), numbers(files_b)
    shared = set(na) & set(nb)
    return bool(shared) and all(abs(na[k] - nb[k]) <= STATE["config"]["agree_tolerance"] for k in shared)


def err(code, message, status, **extra):
    body = {"error": code, "message": message if message else WORDS.get(code, "")}   # v1.1: B's words by default
    body.update(extra)
    return status, body


class Handler(BaseHTTPRequestHandler):
    server_version = "EchoJayMockWizard/1"

    def log_message(self, *a):   # quiet: the tests read STATE["requests"]
        pass

    # --- plumbing -------------------------------------------------------------------------------------------------------------------
    def _record(self, status, body):
        path = urlparse(self.path).path
        if not RECORD["dir"] or path in ("/__mock/scenario", "/__mock/state"):
            return
        if RECORD["scenario"].startswith("_"):   # a timeline no replay can keep (offline gaps against a live POST /runs): pinned by our suite, never shipped
            return
        h = {k: v for k, v in self.headers.items() if k.lower() in ("authorization", "content-type", "content-encoding", "x-ej-content-sha256", "x-ej-client")}
        rb, ctype, enc = getattr(self, "_rb", b""), self.headers.get("Content-Type", ""), self.headers.get("Content-Encoding", "")
        if enc == "gzip":
            try:
                plain = gzip.decompress(rb)
                req = {"gzip_json": json.loads(plain.decode()),
                       "sha_header_matched": hashlib.sha256(plain).hexdigest() == self.headers.get("X-EJ-Content-SHA256", "")}
            except Exception:
                req = {"gzip_undecodable_bytes": len(rb)}
        elif "x-www-form-urlencoded" in ctype:
            req = {"form": {k: v[0] for k, v in parse_qs(rb.decode()).items()}}
        elif rb:
            try:
                req = {"json": json.loads(rb.decode())}
            except ValueError:
                req = {"raw": rb.decode(errors="replace")}
        else:
            req = {}
        with LOCK:
            RECORD["seq"] += 1
            rec = {"scenario": RECORD["scenario"], "seq": RECORD["seq"], "test_hook": path.startswith("/__mock/"),
                   "request": dict({"method": self.command, "path": path, "headers": h}, **req),
                   "response": {"status": status, "body": body}}
            with open(os.path.join(RECORD["dir"], "%05d.json" % RECORD["seq"]), "w") as f:
                json.dump(rec, f, indent=1)

    def _send(self, status, body, headers=None):
        self._record(status, body)
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
        self._rb = self.rfile.read(min(n, limit)) if n > 0 else b""
        return self._rb

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
                s = {k: v for k, v in STATE.items() if k not in ("access", "refresh", "devices", "dead_families", "checked", "needs", "first_check", "runs")}
                s["runs"] = {rid: {k: (sorted(v) if isinstance(v, set) else v) for k, v in r.items()} for rid, r in STATE["runs"].items()}   # v1.1: sets as lists
                s["checked_counts"] = {"%s|%s" % k: len(c) for k, c in STATE["checked"].items()}
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
                return self._send(*err("invalid_client", "", 401))
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
            return self._send(*err("invalid_client", "", 401))
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
        rid = body.get("run_id") or ""
        if not (rid.startswith("run_") and len(rid) == 20):   # v1.1 §2 (ruling 1): run_id REQUIRED on every check
            return self._send(*err("run_id_required", "", 400))
        if len(plugins) > 2000:
            return self._send(*err("bad_request", "", 400))
        # 27 Oct (STAGING 3, run 3: 1,378 plugins in one call, no answer in 30 s): a slow answer, for the client's timeout and back-off.
        # catalogue_delays_s: one delay per call, used up in order; then catalogue_delay_s for every call (default 0)
        with LOCK:
            ds = STATE["config"].get("catalogue_delays_s") or []
            delay = float(ds.pop(0)) if ds else float(STATE["config"].get("catalogue_delay_s", 0) or 0)
            STATE["catalogue_calls"].append({"run_id": rid, "n": len(plugins), "delay_s": delay})
        if delay > 0:
            time.sleep(delay)
        out = []
        acct = self._account()[0]
        with LOCK:
            STATE["checked"].setdefault((acct, rid), set()).update(p.get("plugin_id", "") for p in plugins)
            STATE["first_check"].setdefault((acct, rid), now())
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
                for k in ("level", "category", "reason", "category_source"):
                    if c.get(k) is not None:
                        e[k] = c[k]
                out.append(e)
        # B's category_source (24 Oct, v1.1): known | guessed, omitted when the category is null. The mock's seeded categories are EchoJay's
        # database (known) unless a seed says "guessed"; a category from a tester's served result is known (it was measured)
        for e in out:
            if e.get("category") is not None and "category_source" not in e:
                e["category_source"] = "known"
            elif e.get("category") is None:
                e.pop("category_source", None)
        with LOCK:   # v1.1 §5: the run's queue is the server's - every plugin answered "need" under this run
            STATE["needs"].setdefault((acct, rid), set()).update(e["plugin_id"] for e in out if e["status"] == "need")
        return self._send(200, {"results": out, "categories_version": "mock-2026-10-22"})

    def _new_run(self, acct, body):
        rid = body.get("run_id", "")
        if not rid.startswith("run_") or len(rid) != 20 or parse_time(body.get("started_at")) is None:   # v1.1 §3: started_at required
            return self._send(*err("bad_request", "", 400))
        with LOCK:
            r = STATE["runs"].get(rid)
            if r is not None:
                if r["account_id"] != acct:
                    return self._send(*err("run_conflict", "This run belongs to another account. Sign in with the account you started it with.", 409))
                return self._send(200, {"run_id": rid, "created": False})
            STATE["runs"][rid] = {"account_id": acct, "consent": body.get("consent"), "client": body.get("client"), "results": {}, "accepted_profiles": 0, "completed": False,
                                  "collection": list(body.get("collection") or []), "started_at": parse_time(body.get("started_at")),
                                  "received": set(), "accepted": set(), "new": set(), "covered": set()}
        return self._send(201, {"run_id": rid, "created": True})

    def _result(self, acct, rid, result_id):
        raw = self._raw_body()
        if len(raw) > MAX_COMPRESSED:
            return self._send(*err("too_large", "", 413))
        with LOCK:
            run = STATE["runs"].get(rid)
        if run is None:
            return self._send(*err("run_not_found", "", 404))
        if run["account_id"] != acct:
            return self._send(*err("run_conflict", "", 409))
        try:   # stop decompressing at the limit: no zip bombs
            with gzip.GzipFile(fileobj=__import__("io").BytesIO(raw)) as g:
                plain = g.read(MAX_UNCOMPRESSED + 1)
        except OSError:
            return self._reject(run, result_id, "shape", "One result")
        if len(plain) > MAX_UNCOMPRESSED:
            return self._send(*err("too_large", "", 413))
        sha = hashlib.sha256(plain).hexdigest()
        if sha != self.headers.get("X-EJ-Content-SHA256", ""):
            return self._reject(run, result_id, "hash_mismatch", "")
        with LOCK:
            seen = run["results"].get(result_id)
            if seen is not None:
                if seen == sha:
                    return self._send(200, {"status": "duplicate", "result_id": result_id})
                return self._send(*err("result_conflict", "", 409))
            if len(run["results"]) >= 3000:
                return self._send(*err("too_large", "This run has sent as many results as one run can.", 413))
        try:
            doc = json.loads(plain.decode())
        except ValueError:
            return self._reject(run, result_id, "shape", "One result")
        name = ((doc.get("plugin") or {}).get("name") if isinstance(doc, dict) else None) or "One result"
        if personal_detail(plain.decode(errors="replace")):
            return self._reject(run, result_id, "personal_detail", name)
        bad = validate(doc, rid, result_id, run, acct)
        if bad == "held":   # v1.1 §4 (ruling 3): a schema no validator knows yet - kept, never promoted or served, not counted as accepted
            with LOCK:
                run["results"][result_id] = sha; run["received"].add(result_id); run["covered"].add(doc["plugin"]["plugin_id"]); add_span(run, doc); keep_nonce(doc, result_id)
                STATE["holding"].setdefault(acct, {})[result_id] = dict(doc, _held=True)
            return self._send(201, {"status": "held", "result_id": result_id, "stored": "holding",
                                    "message": "EchoJay can't check %s's measurements yet, so they're kept but not used for now." % name})
        if bad == "held_draft":   # 27 Oct (B): a validated limiter / de-esser draft - accepted and counted, kept as held_draft, never promoted
            with LOCK:
                run["results"][result_id] = sha; run["received"].add(result_id); run["accepted"].add(result_id); add_span(run, doc); keep_nonce(doc, result_id)
                run["covered"].add(doc["plugin"]["plugin_id"])
                STATE["holding"].setdefault(acct, {})[result_id] = dict(doc, _held_draft=True)
            return self._send(201, {"status": "accepted", "result_id": result_id, "stored": "held_draft"})
        if bad:
            return self._reject(run, result_id, bad, name)
        with LOCK:
            run["results"][result_id] = sha; run["received"].add(result_id); run["accepted"].add(result_id); add_span(run, doc); keep_nonce(doc, result_id)
            pid = doc["plugin"]["plugin_id"]; run["covered"].add(pid)
            if doc["kind"] == "profile" and pid in STATE["needs"].get((acct, rid), set()):
                run["new"].add(result_id)
            if doc["kind"] == "profile" and pid not in STATE["catalogue"]:
                run["accepted_profiles"] += 1
                promote(doc, acct, rid, run)
            STATE["holding"].setdefault(acct, {})[result_id] = doc   # the HOLDING area - never served
        return self._send(201, {"status": "accepted", "result_id": result_id, "stored": "holding"})

    def _reject(self, run, result_id, reason, name):
        with LOCK:
            STATE["rejected"].append({"result_id": result_id, "reason": reason})
            run["received"].add(result_id)
            if reason in TAMPERING:
                run["tampered"] = run.get("tampered", 0) + 1
        return self._send(*err("rejected", REJECT_WORDS[reason].format(p=name), 422, reason=reason))

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
            needs = STATE["needs"].get((acct, rid), set())
            uncovered = sorted(needs - run["covered"])   # v1.1 §5: every plugin the server answered "need" has a result (any outcome)
            all_final = not uncovered
            checked = len(set(run.get("collection") or []) & STATE["checked"].get((acct, rid), set()))   # tied by run_id + account
            minimum = STATE["config"]["qualify_minimum"]
            res = {"status": "complete", "results_received": len(run["received"]), "results_accepted": len(run["accepted"]),
                   "results_new": len(run["new"]), "plugins_checked": checked}
            if uncovered:
                res["uncovered"] = len(uncovered)
            claimed_setup = any(r is not run and r.get("promo_code") and r["account_id"] != acct and near_identical(r.get("collection"), run.get("collection"))
                                for r in STATE["runs"].values())
            if acct in STATE["promos"]:
                code = STATE["promos"][acct]
                if run.get("promo_code") == code:   # the same run asking again: the same code, not issued now
                    res["promo"] = {"eligible": True, "code": code, "months": 1, "issued_now": False, "expires_at": "2027-01-31T23:59:59Z", "message": "Thank you - here's your free month."}
                else:
                    res["promo"] = {"eligible": False, "reason": "account_already_claimed", "message": "Your account already has its free month."}
            elif claimed_setup:   # contract v1 §5: once per plugin collection - near-identical to one that already earned a code
                res["promo"] = {"eligible": False, "reason": "collection_already_claimed", "message": "The free month has already been claimed for this setup."}
            elif body.get("kind") != "finished" or not all_final:
                res["promo"] = {"eligible": False, "reason": "not_finished", "message": "The free month needs a finished run - this one stopped early."}
            elif run.get("tampered", 0) > 0:
                res["promo"] = {"eligible": False, "reason": "failed_checks", "message": "Some results didn't pass EchoJay's checks, so the free month can't be given for this run."}
            elif checked < minimum:   # contract v1 §5 (agreed 21 Oct): plugins CHECKED in the run, from the server's own records - not new uploads
                res["promo"] = {"eligible": False, "reason": "below_minimum", "message": "The free month needs at least %d plugins checked in a finished run; this run checked %d." % (minimum, checked)}
            else:
                code = "EJ-FREE-" + secrets.token_hex(4).upper()
                STATE["promos"][acct] = code
                run["promo_code"] = code
                res["promo"] = {"eligible": True, "code": code, "months": 1, "issued_now": True, "expires_at": "2027-01-31T23:59:59Z", "message": "Thank you - here's your free month."}
            return self._send(200, res)

    # --- test hooks (never part of the contract) ------------------------------------------------------------------------------------
    def _hook(self, path, body):
        with LOCK:
            if path == "/__mock/approve":   # the browser step: approve the waiting sign-in(s) for an account
                for d in STATE["devices"].values():
                    if not d["approved"] and not d["denied"]:
                        d["approved"], d["account_id"] = True, body.get("account_id", "acct_mock_a")
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
            if path == "/__mock/scenario":   # labels the exchanges that follow (recording only)
                RECORD["scenario"] = body.get("name", "unlabelled")
                return self._send(200, {})
            if path == "/__mock/verify":   # our own verification certifies (contract v1 §4b)
                sv = STATE["served"].get(body.get("plugin_id", ""))
                if sv:
                    sv["level"], sv["certified_by"] = "certified", "echojay_verification"
                return self._send(200, {"level": sv["level"] if sv else None})
            if path == "/__mock/age_check":   # v1.1 tests: the run's first catalogue check happened N minutes ago (an offline tester's real timeline)
                k = next((k for k in STATE["first_check"] if k[1] == body.get("run_id")), None)
                if k:
                    STATE["first_check"][k] -= 60 * float(body.get("minutes", 0))
                return self._send(200, {"aged": bool(k)})
            if path == "/__mock/agree":   # v1.1 tests: would these two compressor profiles certify each other (ruling 10)?
                return self._send(200, {"agree": comp_agree(body.get("a") or {}, body.get("b") or {})})
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


KINDS = {"profile", "confirmation", "outcome", "needs_category", "other_category", "out_of_scope", "held", "crash", "reference_vote", "bridge_read"}
OUTCOME_STATES = {"needs_licence", "needs_hardware", "demo", "silent", "crashed", "timed_out", "not_measurable", "wont_load", "custom_default",
                  "not_reached", "waves_skipped", "measured", "ready_from_version",
                  "bridged", "bridge_no_match"}   # 26 Oct (asked of B): a VST3 read against its AU twin; the server decides what it inherits


def keep_nonce(doc, result_id):
    for f in (doc.get("files") or {}).values():
        m = f.get("measurement") if isinstance(f, dict) else None
        if isinstance(m, dict) and m.get("nonce"):
            STATE["nonces"].setdefault(m["nonce"], result_id)


def add_span(run, doc):
    s, e = parse_time(doc.get("measure_started_at")), parse_time(doc.get("measured_at"))
    if s is not None and e is not None and e > s:
        run.setdefault("spans", []).append((s, e))


def validate(doc, rid, result_id, run=None, acct=None):
    """The stand-in for the served intake's validators, with B's reason codes (v1.1 §4 / §7): shape, result_id, identity, timestamp,
    validator - or "held" (a file of a schema no validator knows yet) - or None"""
    if not isinstance(doc, dict) or doc.get("schema") != "ej_wizard_result/1":
        return "shape"
    plugin = doc.get("plugin") or {}
    pid, kind = plugin.get("plugin_id", ""), doc.get("kind", "")
    if kind not in KINDS or len(pid.split("|")) != 3 or not plugin.get("name"):
        return "shape"
    if doc.get("run_id") != rid or doc.get("result_id") != result_id:
        return "result_id"
    expect = "res_" + hashlib.sha256(("%s|%s|%s" % (rid, kind, pid)).encode()).hexdigest()[:32]
    if expect != result_id:
        return "result_id"
    # v1.1 §4: created (packing time) and measured_at (when that plugin finished) are required; times within the allowance of the run
    created, measured = parse_time(doc.get("created")), doc.get("measured_at")
    if "measured_at" not in doc:
        return "shape"
    m = parse_time(measured)
    if created is None or m is None:
        return "timestamp"
    t = now()
    if created > t + ALLOW_S or m > t + ALLOW_S:
        return "timestamp"
    if "measure_started_at" not in doc:
        return "shape"
    ms = parse_time(doc.get("measure_started_at"))
    if ms is None or ms > m:
        return "timestamp"
    if kind == "profile" and m - ms < MIN_MEASURE_S.get((doc.get("payload") or {}).get("category"), 0):
        return "timestamp"   # measured faster than that category can be
    if run is not None and m > ms and overlap_with(run.get("spans", []), ms, m) > MAX_OVERLAP:
        return "timestamp"   # more measurements at once than one Mac runs
    if run is not None:
        if run.get("started_at") is not None and created < run["started_at"] - ALLOW_S:   # the WIZARD's start, never the server's run record (offline)
            return "timestamp"
        first = STATE["first_check"].get((acct, rid))
        if first is not None and created < first - ALLOW_S:   # the sanity check: not before this account's first check for the run
            return "timestamp"
    if kind == "outcome" and (doc.get("payload") or {}).get("state") not in OUTCOME_STATES:
        return "shape"
    files = doc.get("files") or {}
    if not isinstance(files, dict):
        return "shape"
    if kind == "profile" and not files:
        return "shape"
    # THE MEASUREMENT IDENTITY (24 Oct, the copy-check ruling): every measured file of a profile says which run and result it was measured for, with a
    # random nonce. Missing -> shape; another run's or result's identity, or a nonce another result already used -> identity (tampering)
    if kind == "profile":
        for name, f in files.items():
            if name == "map.json" or not isinstance(f, dict):
                continue
            m = f.get("measurement")
            if not isinstance(m, dict) or not re.fullmatch(r"[0-9a-f]{32}", str(m.get("nonce", ""))):
                return "shape"
            if m.get("run_id") != rid or m.get("result_id") != result_id:
                return "identity"
            first = STATE["nonces"].get(m["nonce"])
            if first is not None and first != result_id:
                return "identity"
    fp = plugin.get("fingerprint")
    held = draft = False
    for name, f in files.items():
        if not isinstance(f, dict):
            return "shape"
        if name == "map.json":
            if not isinstance(f.get("controls"), dict) or not f["controls"] or f.get("schema") not in MAP_SCHEMAS:
                return "shape"
            uid = str((f.get("identity") or {}).get("uid", "")).lower()
            if not uid or ("|" + uid + "|") not in ("|" + pid.lower() + "|") or (fp and f.get("fp") and f.get("fp") != fp):
                return "shape"   # B's answers (24 Oct): another plugin's map.json is a malformed result ("shape"), not tampering
            continue
        if name.endswith(".tonecheck.json") and "schema" not in f:   # the tone check: known by its name and shape
            if "g_db" not in f:
                return "shape"
            if not any(isinstance(o, dict) and o.get("schema") == "ej_comp_profile/1" for o in files.values()):
                return "shape"   # a tone check that matches no profile
            continue
        fpid = (f.get("plugin") or {}).get("plugin_id")
        if fpid is not None and fpid != pid:
            return "identity"
        ffp = (f.get("plugin") or {}).get("map_fp")
        if fp and ffp and ffp != fp:
            return "identity"
        if f.get("schema") in HELD_SCHEMAS:
            held = True   # B's answers (24 Oct): held by design until B's validator exists
            continue
        if f.get("schema") in DRAFT_SCHEMAS:
            if DRAFT_SCHEMAS[f["schema"]](f):
                return "validator"
            draft = True
            continue
        if kind == "profile" and f.get("schema") not in PROFILE_SCHEMAS:
            held = True   # ruling 3: accepted and HELD
            continue
    cat = (doc.get("payload") or {}).get("category")
    if cat is not None and cat not in CATEGORIES:
        return "validator"
    if kind == "profile":
        for name, f in files.items():   # the stand-in validator: a compressor profile carries its curve
            if isinstance(f, dict) and f.get("schema") == "ej_comp_profile/1" and not isinstance(f.get("amount"), dict):
                return "validator"
    return "held" if held else "held_draft" if draft else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--port-file")
    ap.add_argument("--record", help="write every exchange (raw: real tokens) into this folder - scrub before sharing")
    a = ap.parse_args()
    if a.record:
        os.makedirs(a.record, exist_ok=True)
        RECORD["dir"] = a.record
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
