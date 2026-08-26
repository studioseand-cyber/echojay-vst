#!/usr/bin/env python3
"""Stage 0.5 -- categorise the whole catalogue from PRE-LOAD metadata, offline.

Never loads a plugin. Reads ~/Library/ejmap/scan-cache.xml, asks two arms to
place each product in one of the twelve dial-set categories, and writes
~/Library/ejmap/categories.json for the automated sweep to consume.

    python3 categorise.py                     # everything not yet categorised
    python3 categorise.py --limit 100         # a bite
    python3 categorise.py --report            # the disposition summary
    python3 categorise.py --no-dial-set       # the vocabulary argument, ranked
    python3 categorise.py --unmappable [--accept]
    python3 categorise.py --review            # the disagreements, one at a time
    python3 categorise.py --selftest          # re-run the 40 hand answers

WHY THIS EXISTS, given that the sweep does not need it. A category is ONE LINE
of prompt context in stage 2 (`evidence.py:105`), the proposer's VOCAB is the
UNION of every category's dial set, nothing rejects an empty category, and the
client never reads one at dial time. So a wrong category cannot produce a wrong
dial and cannot even produce an out-of-vocabulary semantic.

What this pass is really for is deciding WHAT NOT TO LOAD. Measured on a random
150: a third of the catalogue is a processor the twelve categories do not cover,
or not a processor at all. That is ~358 of 1,073 products the sweep never opens.

THE TWO REFUSALS ARE NOT ONE BUCKET.

  not_a_processor   analysers, meters, matrix routers, generators. Nothing to
                    dial, ever. Proposes an unmappable mark -- a PROPOSAL, in a
                    batch, never an auto-write, because unmappable is carried
                    forward across versions and is meant to be hard to undo.

  none              a real processor no category fits: filters, imaging,
                    modulation, pitch shifting. This is a statement about the
                    TWELVE-CATEGORY VOCABULARY, not about the plugin. Recorded as
                    `no_dial_set` and kept queryable, because grouped by kind it
                    is the ranked argument for which dial set to add next.

UNVALIDATED, AND SAYING SO. The category itself was checked against 40 hand
answers (both-confident agreement matched 33/33 and 34/34 across two runs). The
6%/27% refusal split has NO hand-marked set to check against -- marks.json holds
zero unmappable entries. The batch review in --unmappable is where that split
gets validated; until it runs, it is a proposal, not a measurement.
"""
import argparse, collections, concurrent.futures, datetime, hashlib, json, os, re
import sys, threading, xml.etree.ElementTree as ET

ROOT = os.path.expanduser("~/Library/ejmap")
OPUS, GPT = "claude-opus-5", "gpt-5.5"
TOOL_VERSION = "categorise/1"
BATCH = 25

REFUSALS = ("none", "not_a_processor")

CATEGORIES = ["compressor", "limiter", "eq", "de-esser", "delay", "reverb",
              "saturation", "gate", "transient_shaper", "channel_strip", "amp_sim",
              "pitch"]

SYSTEM = """You categorise audio plug-ins for a tool that dials their controls.

You are given ONLY pre-load metadata: the plug-in's name, its vendor, its format,
and -- when the format declares one -- the vendor's own subcategory string. You
have NOT seen its parameters. Answer from what you know about these products.

Choose exactly one of:
""" + "\n".join("  " + c for c in CATEGORIES) + """

or one of these two refusals:

  none        it processes audio but none of the categories fit (modulation,
              imaging, restoration, filtering, pitch shifting and
              harmonising without key/scale correction...)
  not_a_processor
              it does not process audio at all, or has nothing worth dialling
              (analysers, meters, key detectors, alignment utilities,
              routing helpers, generators)

RULES
- A "(m)" or "(s)" suffix is a mono/stereo variant of the same product. It never
  changes the category.
- channel_strip means a MULTI-STAGE console channel (EQ + dynamics in one).
  A compressor with a tone control is still a compressor.
- amp_sim means a guitar/bass amplifier or cabinet emulation.
- saturation covers tape, tube, transformer and distortion colour boxes.
- pitch means a pitch CORRECTOR that retunes the voice to a key or scale
  (e.g. Auto-Tune, Melodyne) -- its surface includes retune speed, key,
  scale, flex-tune. A plugin sold under a tuner's brand that is really an
  EQ or a compressor is eq or compressor, not pitch. A pitch shifter or
  harmonizer with no key/scale correction is not pitch.
- If you do not recognise the product, say so with confidence "low" and your
  best guess. Do NOT invent a category from the name alone if the name is
  generic.

confidence is "high" only when you are confident you know this specific product.

WHEN you answer none or not_a_processor, also give `kind`: two or three words
for what it actually is ("stereo imager", "spectrum analyser", "pitch shifter",
"multiband filter"). Use the SAME words for the same kind of thing across
plug-ins, because these get grouped. Omit `kind` otherwise.

Reply with JSON only:
{"plugins":[{"name":"...","category":"...","confidence":"high|low","kind":"..."}]}
Return one entry per plugin given, in the same order."""

SYSTEM_SHA = hashlib.sha256(SYSTEM.encode()).hexdigest()[:12]

_WRITE_LOCK = threading.Lock()


# ---------------------------------------------------------------------------
# the catalogue

_VARIANT = re.compile(r"\s*\((?:m|s|mono|stereo|\d+->\d+)\)\s*$", re.I)


def base_name(n):
    """Collapse Waves' variant suffixes. A (m)/(s) pair is ONE product and
    paying twice to be told so twice is waste -- 1,819 rows become 1,073."""
    return _VARIANT.sub("", n.strip()).strip()


def product_key(name, vendor):
    return f"{base_name(name).lower()}|{vendor.strip().lower()}"


def mark_product_key(fmt, unique_id_hex):
    """Mirrors ejmap::Marks::productKey -- format|hex(uniqueId), version dropped
    on purpose, because unmappable is a decision about a PRODUCT.

    THE ATTRIBUTE IS ALREADY HEX. juce_PluginDescription.cpp writes both
    `uniqueId` and `uid` with String::toHexString, and the C++ mark key is
    toHexString(uniqueId) -- so the key IS the attribute, and converting it is
    the bug. Most values look decimal ('70194649' is hex), which is exactly why
    a decimal reading survives a sample and dies on the first row with a letter
    in it. It died on '26a5d6d'.
    """
    h = str(unique_id_hex).strip().lower().lstrip("0") or "0"
    return f"{fmt}|{h}"


def scan_products(root=ROOT):
    """Every scanned row, collapsed to products, with the format's own
    subcategory inherited across AU/VST3 siblings.

    Every VST3 declares one and no AudioUnit does, so the inheritance is worth
    493 rows for free. It never DECIDES -- 'Fx|Dynamics' spans compressor,
    limiter, gate, de-esser and transient_shaper -- it constrains the prompt."""
    path = os.path.join(root, "scan-cache.xml")
    rows = [p.attrib for p in ET.parse(path).getroot().iter("PLUGIN")]

    declared = {}
    for r in rows:
        if r.get("category", "").strip():
            declared[product_key(r["name"], r["manufacturer"])] = r["category"].strip()

    prods = {}
    for r in rows:
        k = product_key(r["name"], r["manufacturer"])
        fmt = "AudioUnit" if r["file"].startswith("AudioUnit:") else "VST3"
        p = prods.setdefault(k, {"key": k, "name": base_name(r["name"]),
                                 "vendor": r["manufacturer"], "formats": [],
                                 "declared": declared.get(k, ""),
                                 "members": [], "mark_keys": []})
        p["members"].append(r["file"])
        if fmt not in p["formats"]:
            p["formats"].append(fmt)
        # uniqueId, never uid: Marks::productKey reads d.uniqueId, and uid is
        # JUCE's deprecatedUid -- a different number on the same plugin.
        mk = mark_product_key(fmt, r.get("uniqueId") or "0")
        if mk not in p["mark_keys"]:
            p["mark_keys"].append(mk)
    return prods


# ---------------------------------------------------------------------------
# the two arms

def _parse(text):
    text = text.strip()
    fenced = re.search(r"```(?:json)?\s*(.*?)```", text, re.S)
    if fenced:
        text = fenced.group(1).strip()
    i, j = text.find("{"), text.rfind("}")
    return json.loads(text[i:j + 1])["plugins"]


def user_msg(batch):
    lines = []
    for p in batch:
        s = (f"- {p['name']}  |  vendor: {p['vendor']}  |  "
             f"format: {'/'.join(p['formats'])}")
        if p["declared"]:
            s += f"  |  vendor subcategory: {p['declared']}"
        lines.append(s)
    return "Categorise these plug-ins:\n\n" + "\n".join(lines)


def ask_opus(client, user):
    r = client.messages.create(model=OPUS, max_tokens=8000, system=SYSTEM,
                               output_config={"effort": "high"},
                               messages=[{"role": "user", "content": user}])
    return _parse("".join(b.text for b in r.content if b.type == "text"))


def ask_gpt(client, user):
    r = client.chat.completions.create(
        model=GPT, max_completion_tokens=8000,
        response_format={"type": "json_object"},
        messages=[{"role": "system", "content": SYSTEM},
                  {"role": "user", "content": user}])
    return _parse(r.choices[0].message.content)


def with_retry(fn, *a, tries=3):
    import time
    last = None
    for n in range(tries):
        try:
            return fn(*a), None
        except Exception as e:                      # noqa: BLE001
            last = e
            time.sleep(2 * (n + 1))
    return None, str(last)[:160]


# ---------------------------------------------------------------------------
# the gate

def decide(a, b):
    """Four dispositions, and the gate can only refuse.

    A hedged agreement is still SWEPT, and the reason is upstream: the category
    is a prompt nudge over an unrestricted vocabulary, so a hedged-but-agreed
    category is strictly better than the `unknown` the proposer already
    tolerates. It is recorded as hedged so the review pile is identifiable --
    the hedged rows are 8.7% of the catalogue and they are read afterwards, in
    one sitting, not in the loop."""
    ac, bc = a.get("category"), b.get("category")
    if ac is None or bc is None:
        return "review", None, "an arm returned nothing"
    if ac != bc:
        # BOTH REFUSED, they just disagree on WHICH refusal. The sweep does not
        # need this decided -- it never loads the plugin either way -- so this
        # blocks only the unmappable question, not the run. 14 of the 87 real
        # disagreements are this shape.
        if ac in REFUSALS and bc in REFUSALS:
            return "review", None, f"both refused, differently: {ac} vs {bc}"
        return "review", None, f"arms disagree: {ac} vs {bc}"
    confident = a.get("confidence") == "high" and b.get("confidence") == "high"
    if ac == "not_a_processor":
        return "not_a_processor", None, ""
    if ac == "none":
        return "no_dial_set", None, ""
    if ac not in CATEGORIES:
        return "review", None, f"both said {ac!r}, which is not a category"
    return "sweep", ac, "" if confident else "hedged: one arm was not confident"


def merge(prod, a, b, at):
    disp, cat, why = decide(a, b)
    confident = (a.get("confidence") == "high" and b.get("confidence") == "high")
    kinds = [x.get("kind") for x in (a, b) if x.get("kind")]
    return {
        "name": prod["name"], "vendor": prod["vendor"],
        "formats": prod["formats"], "declared": prod["declared"],
        "members": prod["members"], "mark_keys": prod["mark_keys"],
        "arms": [{"model": OPUS, **{k: a.get(k) for k in ("category", "confidence", "kind")}},
                 {"model": GPT, **{k: b.get(k) for k in ("category", "confidence", "kind")}}],
        "category": cat,
        "confidence": "high" if confident else "hedged",
        "kind": kinds[0] if kinds else None,
        "kind_agreed": len(kinds) == 2 and kinds[0].strip().lower() == kinds[1].strip().lower(),
        "disposition": disp, "why": why, "at": at,

        # THE ONE FIELD THE SWEEP READS TO DECIDE WHETHER TO OPEN A PLUGIN.
        # A product is skipped when neither arm offered a category -- which
        # covers both settled refusals AND the disagreement between two
        # refusals, because "which refusal" is a question for the marks review,
        # not for the loader.
        "refused_by_both": (a.get("category") in REFUSALS
                            and b.get("category") in REFUSALS),
    }


# ---------------------------------------------------------------------------
# the file

def sweepable(p):
    """Whether the automated sweep opens this plugin. ONE definition, read by
    the report here and by ejmap --sweep, so the two cannot drift."""
    return p.get("disposition") == "sweep" and not p.get("refused_by_both")


def cat_file(root=ROOT):
    return os.path.join(root, "categories.json")


def load_cats(root=ROOT):
    p = cat_file(root)
    if not os.path.exists(p):
        return {"run": {}, "products": {}}
    return json.load(open(p))


def save_cats(doc, root=ROOT):
    tmp = cat_file(root) + ".tmp"
    json.dump(doc, open(tmp, "w"), indent=1)
    os.replace(tmp, cat_file(root))


# ---------------------------------------------------------------------------

def run(args):
    import anthropic, openai
    clients = {"a": anthropic.Anthropic(), "o": openai.OpenAI()}

    prods = scan_products(args.root)
    doc = load_cats(args.root)
    todo = [p for k, p in sorted(prods.items())
            if args.force or k not in doc["products"]]
    if args.only:
        todo = [p for p in todo if args.only.lower() in p["name"].lower()]
    if args.limit:
        todo = todo[:args.limit]

    print(f"{len(prods)} product(s) in the scan, {len(doc['products'])} already "
          f"categorised, {len(todo)} to do")
    if not todo:
        return

    doc["run"] = {"models": [OPUS, GPT], "prompt_sha": SYSTEM_SHA,
                  "tool": TOOL_VERSION, "at": datetime.datetime.now()
                  .astimezone().isoformat(timespec="seconds")}

    batches = [todo[i:i + BATCH] for i in range(0, len(todo), BATCH)]
    done = [0]

    def one(batch):
        user = user_msg(batch)
        with concurrent.futures.ThreadPoolExecutor(2) as ex:
            fa = ex.submit(with_retry, ask_opus, clients["a"], user)
            fb = ex.submit(with_retry, ask_gpt, clients["o"], user)
            (a_ans, a_err), (b_ans, b_err) = fa.result(), fb.result()

        # A FAILED ARM WRITES NOTHING. Presence means done, so a transient 529
        # that wrote a file would record an outage as a permanent result -- the
        # defect that put 11 of 33 plugins at 100% escalated on 4 Aug.
        if a_err or b_err:
            print(f"  batch of {len(batch)} FAILED, nothing written "
                  f"({a_err or ''}{b_err or ''})")
            return

        am = {x.get("name"): x for x in a_ans}
        bm = {x.get("name"): x for x in b_ans}
        at = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        with _WRITE_LOCK:
            fresh = load_cats(args.root)
            fresh["run"] = doc["run"]
            for i, p in enumerate(batch):
                a = am.get(p["name"]) or (a_ans[i] if i < len(a_ans) else {})
                b = bm.get(p["name"]) or (b_ans[i] if i < len(b_ans) else {})
                fresh["products"][p["key"]] = merge(p, a, b, at)
            save_cats(fresh, args.root)
        done[0] += len(batch)
        print(f"  {done[0]}/{len(todo)}")

    with concurrent.futures.ThreadPoolExecutor(args.workers) as ex:
        list(ex.map(one, batches))

    report(args)


# ---------------------------------------------------------------------------
# reading it back

def report(args):
    doc = load_cats(args.root)
    ps = doc["products"]
    if not ps:
        print("nothing categorised yet")
        return
    c = collections.Counter(p["disposition"] for p in ps.values())
    hedged = sum(1 for p in ps.values()
                 if p["disposition"] == "sweep" and p["confidence"] == "hedged")
    n = len(ps)
    print(f"\n{n} product(s) categorised  (prompt {doc['run'].get('prompt_sha')})\n")
    for k in ("sweep", "no_dial_set", "not_a_processor", "review"):
        v = c.get(k, 0)
        print(f"  {v:5d}  {100*v/n:5.1f}%  {k}")
    print(f"\n  of the {c.get('sweep', 0)} to sweep, {hedged} are HEDGED "
          f"(one arm was not confident) -- swept, and listed here to read after")
    print(f"\n  categories chosen:")
    for k, v in collections.Counter(p["category"] for p in ps.values()
                                    if p["category"]).most_common():
        print(f"    {v:5d}  {k}")
    both_ref = sum(1 for p in ps.values()
                   if p["disposition"] == "review" and p.get("refused_by_both"))
    print(f"\n  {c.get('review', 0)} need a decision, of which {both_ref} are BOTH ARMS")
    print(f"  REFUSING and differing only on which refusal -- the sweep skips those")
    print(f"  regardless, so only {c.get('review', 0) - both_ref} block the run.")
    print(f"  python3 categorise.py --review")

    skipped = sum(1 for p in ps.values() if not sweepable(p))
    print(f"\n  the sweep would open {n - skipped} of {n} product(s) and skip {skipped}")


def no_dial_set(args):
    """The vocabulary argument. 'none' is a statement about the TWELVE
    CATEGORIES, not about the plugin, and grouped by kind it says which dial set
    to add next and how big each one is."""
    doc = load_cats(args.root)
    rows = [p for p in doc["products"].values() if p["disposition"] == "no_dial_set"]
    if not rows:
        print("no no_dial_set products recorded")
        return
    by = collections.defaultdict(list)
    for r in rows:
        by[(r.get("kind") or "(unlabelled)").strip().lower()].append(r)
    print(f"\n{len(rows)} product(s) both arms call a real processor that none of "
          f"the {len(CATEGORIES)} categories fit.")
    print("Ranked by how many products a new dial set would unlock:\n")
    for kind, rs in sorted(by.items(), key=lambda kv: -len(kv[1])):
        agreed = sum(1 for r in rs if r["kind_agreed"])
        print(f"  {len(rs):5d}  {kind:32s} ({agreed} with both arms naming it the same)")
        if args.verbose:
            for r in sorted(rs, key=lambda x: x["name"])[:args.verbose]:
                print(f"           {r['name'][:44]:44s} {r['vendor'][:22]}")
    print("\nThis is an asset, not a dismissal: none of these is marked unmappable.")


def unmappable(args):
    """A PROPOSAL, reviewed in a batch. Never an auto-write.

    unmappable keys on the PRODUCT with the version dropped and is carried
    forward across updates on purpose, so it is meant to be hard to undo. And
    unlike the category -- which was checked against 40 hand answers -- this
    split has NO hand-marked set to check against: marks.json holds zero
    unmappable entries. This review is where it gets validated."""
    doc = load_cats(args.root)
    rows = sorted((p for p in doc["products"].values()
                   if p["disposition"] == "not_a_processor"),
                  key=lambda r: (r.get("kind") or "", r["name"]))
    if not rows:
        print("no not_a_processor proposals recorded")
        return
    print(f"\n{len(rows)} product(s) both arms say are not audio processors.")
    print("Proposed as unmappable marks. NOTHING is written without --accept.\n")
    for r in rows:
        conf = "both confident" if r["confidence"] == "high" else "one arm hedged"
        print(f"  {r['name'][:40]:40s} {r['vendor'][:20]:20s} "
              f"{(r.get('kind') or '?')[:24]:24s} ({conf})")

    if not args.accept:
        print("\nDry run. Add --accept to write these to marks.json.")
        return

    marks_path = os.path.join(args.root, "marks.json")
    marks = json.load(open(marks_path)) if os.path.exists(marks_path) \
        else {"issues": {}, "unmappable": {}}
    marks.setdefault("issues", {})
    marks.setdefault("unmappable", {})
    at = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    added = 0
    for r in rows:
        for k in r["mark_keys"]:
            if k in marks["unmappable"]:
                continue
            # Provenance names the tool AND the prompt, so a later disagreement
            # can find what produced it. Same shape as every other record here.
            marks["unmappable"][k] = {
                "by": f"{TOOL_VERSION} ({OPUS}+{GPT}, prompt {SYSTEM_SHA})",
                "at": at}
            added += 1
    tmp = marks_path + ".tmp"
    json.dump(marks, open(tmp, "w"), indent=1)
    os.replace(tmp, marks_path)
    print(f"\nwrote {added} unmappable mark(s) to {marks_path}")


def review(args):
    doc = load_cats(args.root)
    rows = sorted((p for p in doc["products"].values()
                   if p["disposition"] == "review"), key=lambda r: r["name"])
    if not rows:
        print("nothing to review")
        return
    print(f"{len(rows)} disagreement(s). Type a category, "
          f"'n' (no dial set), 'x' (not a processor), 's' (skip) or 'q'.\n")
    for i, r in enumerate(rows):
        a, b = r["arms"]
        print(f"[{i+1}/{len(rows)}] {r['name']}   {r['vendor']}"
              f"{'   ' + r['declared'] if r['declared'] else ''}")
        print(f"     {a['model']}: {a['category']} ({a['confidence']})"
              f"     {b['model']}: {b['category']} ({b['confidence']})")
        ans = input("     > ").strip()
        if ans == "q":
            break
        if ans in ("", "s"):
            continue
        if ans == "n":
            r["disposition"], r["category"] = "no_dial_set", None
        elif ans == "x":
            r["disposition"], r["category"] = "not_a_processor", None
        elif ans in CATEGORIES:
            r["disposition"], r["category"] = "sweep", ans
        else:
            print(f"     not a category: {ans}")
            continue
        r["confidence"] = "human"
        r["why"] = "decided by hand at review"
        r["at"] = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        save_cats(doc, args.root)
    print("saved")


def selftest(args):
    """The acceptance test: re-run the 40 plugins the mapper categorised by hand
    and assert the both-confident agreements still match. Costs an API call, so
    it is explicit rather than part of the always-on gate."""
    import glob
    import anthropic, openai
    prods = scan_products(args.root)
    truth = []
    for f in glob.glob(os.path.join(args.root, "maps", "*.json")):
        d = json.load(open(f))
        i = d["identity"]
        k = product_key(i["name"], i.get("vendor") or "")
        p = prods.get(k) or {"name": i["name"], "vendor": i.get("vendor") or "?",
                             "formats": [i.get("format") or "?"], "declared": ""}
        truth.append((dict(p), d.get("category")))
    if not truth:
        print("no maps to check against")
        return 2

    clients = {"a": anthropic.Anthropic(), "o": openai.OpenAI()}
    batches = [truth[i:i + BATCH] for i in range(0, len(truth), BATCH)]
    conf_n = conf_ok = agree = 0
    misses = []
    for batch in batches:
        user = user_msg([p for p, _ in batch])
        (a_ans, ae), (b_ans, be) = (with_retry(ask_opus, clients["a"], user),
                                    with_retry(ask_gpt, clients["o"], user))
        if ae or be:
            print(f"selftest: an arm failed ({ae or be}); INCONCLUSIVE")
            return 2
        am = {x.get("name"): x for x in a_ans}
        bm = {x.get("name"): x for x in b_ans}
        for i, (p, hand) in enumerate(batch):
            a = am.get(p["name"]) or (a_ans[i] if i < len(a_ans) else {})
            b = bm.get(p["name"]) or (b_ans[i] if i < len(b_ans) else {})
            if a.get("category") == b.get("category"):
                agree += 1
            if (a.get("category") == b.get("category")
                    and a.get("confidence") == b.get("confidence") == "high"):
                conf_n += 1
                if a.get("category") == hand:
                    conf_ok += 1
                else:
                    misses.append((p["name"], hand, a.get("category")))
    print(f"selftest over {len(truth)} hand-categorised maps:")
    print(f"  agreed                     {agree}/{len(truth)}")
    print(f"  both-confident agreements  {conf_n}")
    print(f"  ...matching the hand answer {conf_ok}/{conf_n}")
    for name, hand, got in misses:
        print(f"    MISS  {name:34s} hand={hand:18s} both={got}")
    if conf_n and conf_ok == conf_n:
        print("PASS: every both-confident agreement matches the hand answer")
        return 0
    print("FAIL: a both-confident agreement disagrees with the hand answer")
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only", default=None)
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--no-dial-set", action="store_true", dest="nodialset")
    ap.add_argument("--unmappable", action="store_true")
    ap.add_argument("--accept", action="store_true")
    ap.add_argument("--review", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--verbose", type=int, default=0,
                    help="with --no-dial-set, list up to N products per kind")
    args = ap.parse_args()

    if args.selftest:   sys.exit(selftest(args))
    if args.report:     return report(args)
    if args.nodialset:  return no_dial_set(args)
    if args.unmappable: return unmappable(args)
    if args.review:     return review(args)
    run(args)


if __name__ == "__main__":
    main()
