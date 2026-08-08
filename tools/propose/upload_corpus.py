#!/usr/bin/env python3
"""Put the local corpus on the server: maps first, then proposals.

    python3 upload_corpus.py --dry-run      # what would be sent, and why
    python3 upload_corpus.py                # send it
    python3 upload_corpus.py --only-proposals

RESUME BY ASKING THE SERVER, NEVER BY A LOCAL LEDGER. queue.json records what
this machine did; it is not the server's state, and on 8 Aug the two agreed at
22 while the server was also serving a map that had been withdrawn locally
three days earlier. So every run starts by reading GET /api/params/maps and
uploading the difference. A crash costs the request in flight and nothing else.

THE BIGGEST MAP GOES FIRST, on purpose. TH-U Slate is 4.19 MB and Repeater is
3.90 MB, against a serverless request-body limit I am not going to assert from
memory. Sending the largest one as the FIRST request turns an unknown into a
measurement in ten seconds, rather than into an hour of uploads followed by two
failures at the end.

WHAT IT WILL NOT DO:
  * force. A 409 would_drop_params means a controls-only map would replace one
    with params; the fix is a graft, not a flag, and it prints the stored
    semantics so the graft can be composed.
  * invent. A map that fails is reported and left; nothing is retried into a
    different shape.
"""
import argparse, collections, glob, json, os, sys, time, urllib.error, urllib.parse, urllib.request

ROOT = os.path.expanduser("~/Library/ejmap")
BASE = "https://www.echojay.ai"
# Cloudflare returns 403 (error 1010) to the default python-urllib agent. That
# is a bot check, not auth and not data -- it cost an hour on 8 Aug being read
# as "the endpoint is gone".
UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124 Safari/537.36")


def token():
    cfg = json.load(open(os.path.join(ROOT, "config.json")))
    t = cfg.get("mapper_token") or cfg.get("ingest_token") or cfg.get("token")
    if not t:
        sys.exit("no token in ~/Library/ejmap/config.json")
    return t


def post(path, body, tok, extra=None):
    data = json.dumps(body).encode()
    h = {"Content-Type": "application/json", "X-EJMap-Token": tok, "User-Agent": UA}
    h.update(extra or {})
    req = urllib.request.Request(BASE + path, data=data, method="POST", headers=h)
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, json.loads(r.read() or b"{}"), len(data)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try: return e.code, json.loads(raw or b"{}"), len(data)
        except Exception: return e.code, {"raw": raw[:200].decode("utf8", "replace")}, len(data)
    except Exception as e:                                       # noqa: BLE001
        return 0, {"error": str(e)}, len(data)


def _ask(batch, sep):
    q = urllib.parse.quote(sep.join(batch))
    url = f"{BASE}/api/params/maps?identities={q}"
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}),
                                timeout=90) as r:
        return json.load(r)


def server_state(idents, sep=None):
    """What the server holds, asked of the server.

    NEWLINE, NOT COMMA -- a version string may contain a comma and OTT's does.
    But the server only accepts newline once THAT fix is deployed, and an
    undeployed server does not say so: it splits the whole newline-joined batch
    on commas, matches nothing, and returns [] for every key. Which is also how
    it says "unmapped".

    That is the original bug wearing the uploader's clothes, and it showed up
    on the first dry run: `server: 0 of them already stored` against a server
    holding 22. Reading it as truth would have re-sent the entire corpus.

    So the encoding is DETECTED, not assumed: ask with newline, and if the
    reply contains none of the keys asked for, ask again with comma. A server
    that understood the question echoes the identities back.
    """
    known, maps, chosen = {}, {}, sep
    for i in range(0, len(idents), 200):
        batch = idents[i:i + 200]
        if chosen is None:
            d = _ask(batch, "\n")
            got = d.get("identities") or {}
            if not any(k in got for k in batch):
                d = _ask(batch, ",")
                chosen = ","
                print("  note: this server splits identities on COMMA -- the newline "
                      "fix is not deployed yet. Falling back; identities containing a "
                      "comma cannot be asked about until it is.")
            else:
                chosen = "\n"
        else:
            d = _ask(batch, chosen)
        known.update(d.get("identities") or {})
        maps.update(d.get("maps") or {})
        time.sleep(0.2)
    return known, maps


def main():
    global BASE, ROOT
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only-proposals", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    BASE, ROOT = args.base, args.root
    tok = token()

    local = {}
    for f in glob.glob(os.path.join(ROOT, "maps", "*.json")):
        d = json.load(open(f))
        i = d.get("identity") or {}
        d["_ident"] = f"{i.get('format')}|{(i.get('uid') or '').lower()}|{i.get('version')}"
        d["_bytes"] = os.path.getsize(f)
        local[d["fp"]] = d
    print(f"local: {len(local)} maps")

    print("asking the server what it holds ...")
    known, served_maps = server_state(sorted({d["_ident"] for d in local.values()}))
    have = set(served_maps)
    print(f"server: {len(have)} of them already stored\n")

    todo = [d for fp, d in local.items() if fp not in have]
    # Largest first: an unknown body-size ceiling should fail on request one.
    todo.sort(key=lambda d: -d["_bytes"])
    if args.limit:
        todo = todo[:args.limit]

    if not args.only_proposals:
        print(f"MAPS: {len(todo)} to send, largest first "
              f"({todo[0]['_bytes']/1e6:.2f} MB {todo[0]['identity']['name']})"
              if todo else "MAPS: nothing to send")
        if args.dry_run:
            for d in todo[:10]:
                print(f"   would send {d['_bytes']/1e6:6.2f} MB  {d['identity']['name']}")
            print(f"   ... {len(todo)} total, {sum(d['_bytes'] for d in todo)/1e6:.1f} MB")
        else:
            out = collections.Counter()
            for n, d in enumerate(todo, 1):
                payload = {k: v for k, v in d.items() if not k.startswith("_")}
                st, body, nbytes = post("/api/params/ejmap", payload, tok)
                out[st] += 1
                if st != 200 or n <= 3 or n % 100 == 0:
                    note = ""
                    if st == 409 and body.get("error") == "would_drop_params":
                        note = (f"  REFUSED: would drop {body.get('existing_params')} stored "
                                f"param(s) {body.get('existing_semantics')} -- graft, do not force")
                    elif st != 200:
                        note = f"  {json.dumps(body)[:140]}"
                    print(f"  [{n:4}/{len(todo)}] {st} {nbytes/1e6:5.2f}MB "
                          f"{d['identity']['name'][:34]:34}{note}", flush=True)
            print(f"\n  maps by status: {dict(out)}")

    # ---- proposals, after the maps, because one without a map is refused ----
    props = [f for f in glob.glob(os.path.join(ROOT, "proposals", "*.json"))
             if not os.path.isdir(f)]
    print(f"\nPROPOSALS: {len(props)} local")
    if args.dry_run:
        print(f"   would send {sum(os.path.getsize(f) for f in props)/1e6:.1f} MB")
        return 0
    out = collections.Counter()
    for n, f in enumerate(sorted(props), 1):
        d = json.load(open(f))
        st, body, _ = post("/api/params/proposals", d, tok)
        out[st] += 1
        if st != 200 or n % 200 == 0:
            print(f"  [{n:4}/{len(props)}] {st} {d.get('plugin','?')[:34]:34}"
                  f"{'' if st == 200 else '  ' + json.dumps(body)[:120]}", flush=True)
    print(f"\n  proposals by status: {dict(out)}")

    # ---- verify against the server, not against this script's own counters ---
    print("\nVERIFYING by re-reading the server ...")
    known2, maps2 = server_state(sorted({d["_ident"] for d in local.values()}))
    print(f"  maps stored:      {len(maps2)} of {len(local)}")
    missing = [local[fp]["identity"]["name"] for fp in local if fp not in maps2]
    if missing:
        print(f"  MISSING {len(missing)}: {', '.join(missing[:8])}"
              + (" ..." if len(missing) > 8 else ""))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
