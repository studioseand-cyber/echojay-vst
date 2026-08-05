#!/usr/bin/env python3
"""Stage 2 -- propose semantics over swept maps, offline, in batch.

Reads ~/Library/ejmap/maps/*.json. NEVER loads a plugin. Two arms see the same
prompt, neither sees the other's answer. A row is accepted only when both agree,
both are confident, the unit family holds, and no other control on the same
plugin claims the same semantic. Everything else is written out as an escalation
for review.

RESUMABLE BY FILE LAYOUT, not by a mechanism: one proposal file per fingerprint,
and its presence means done. A crash costs the plugin in flight.

    python3 propose.py                     # every unproposed map
    python3 propose.py --only CLA-76       # substring match on the name
    python3 propose.py --audit             # re-derive human answers, score them
    python3 propose.py --force             # redo maps already proposed
"""
import argparse, concurrent.futures, datetime, json, os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evidence import candidates, format_for_prompt, unit_family_conflict, VOCAB
from prompt import SYSTEM, SYSTEM_SHA

OPUS = "claude-opus-5"
GPT = "gpt-5.5"
TOOL_VERSION = "propose/1"
AUDIT = []


# --------------------------------------------------------------------------
# the two arms

def _parse(text):
    text = text.strip()
    fenced = re.search(r"```(?:json)?\s*(.*?)```", text, re.S)
    if fenced:
        text = fenced.group(1).strip()
    i, j = text.find("{"), text.rfind("}")
    if i >= 0 and j > i:
        text = text[i:j + 1]
    return json.loads(text)["controls"]


def ask_opus(client, user, effort):
    r = client.messages.create(model=OPUS, max_tokens=16000, system=SYSTEM,
                               output_config={"effort": effort},
                               messages=[{"role": "user", "content": user}])
    return _parse("".join(b.text for b in r.content if b.type == "text"))


def ask_gpt(client, user):
    r = client.chat.completions.create(
        model=GPT, max_completion_tokens=16000,
        response_format={"type": "json_object"},
        messages=[{"role": "system", "content": SYSTEM},
                  {"role": "user", "content": user}])
    return _parse(r.choices[0].message.content)


def with_retry(fn, *a):
    """529 Overloaded is routine at this fan-out and is retryable. The first
    run of this tool lost 11 of 33 plugins to it, so the backoff is generous:
    an arm that fails costs the whole plugin, because one answer is not an
    agreement.
    """
    last = None
    for attempt in range(6):
        try:
            return fn(*a), None
        except Exception as e:                                  # noqa: BLE001
            last = e
            time.sleep(min(60, 3 * (2 ** attempt)) + random.uniform(0, 2))
    return None, str(last)


# --------------------------------------------------------------------------
# the merge -- this is the whole decision, and it is deliberately boring

def merge(rows, a_ans, b_ans):
    """Both arms' answers against the evidence.

    Returns (accepted, escalated, declined).

    Four gates, each of which can only ever REFUSE:
      1. both arms named the same semantic
      2. both arms were confident
      3. the semantic's unit family does not contradict the measured unit
      4. no other control on this plugin was accepted for the same semantic

    Gate 4 exists because `params` is a dict keyed by semantic: two rows claiming
    one key would silently overwrite, and duplicateIndexConflicts does not catch
    it -- that rule catches one INDEX claimed by two semantics, the reverse case.
    """
    a = {r.get("name"): r for r in a_ans}
    b = {r.get("name"): r for r in b_ans}

    accepted, escalated, declined = [], [], []
    for ev in rows:
        name = ev["name"]
        ra, rb = a.get(name), b.get(name)
        a_sem = (ra or {}).get("semantic", "__absent__") or "none"
        b_sem = (rb or {}).get("semantic", "__absent__") or "none"
        a_conf = (ra or {}).get("confidence", "low") or "low"
        b_conf = (rb or {}).get("confidence", "low") or "low"

        arms = [{"model": OPUS, "semantic": a_sem, "confidence": a_conf},
                {"model": GPT, "semantic": b_sem, "confidence": b_conf}]
        row = dict(index=ev["index"], name=name, evidence=ev, arms=arms)

        # Absence is checked FIRST. A silently dropped control is a different
        # failure from a disagreement -- one is the arm not answering, the other
        # is two answers that differ -- and reporting the first as the second
        # would hide a truncated response behind a plausible-looking dispute.
        if a_sem == "__absent__" or b_sem == "__absent__":
            escalated.append({**row, "why": "an arm did not answer this control"})
        elif a_sem != b_sem:
            escalated.append({**row, "why": "arms disagree"})
        elif a_sem == "none":
            # A CONFIDENT, AGREED "none" is an answer, not an escalation. Most of
            # a plugin's controls are bypasses, in/out switches and band
            # selectors, and putting every one of them in front of a human is how
            # a review pile becomes 530 rows of "this is a bypass". Where either
            # arm hedged, it still escalates -- the one both-agree-"none" error in
            # the hold-out (spiff `decay`, truly release_ms) was low/low.
            if a_conf == "high" and b_conf == "high":
                declined.append({**row, "why": "both arms confidently say no semantic applies"})
            else:
                escalated.append({**row, "why": "both arms say none, but not confidently"})
        elif a_sem not in VOCAB:
            escalated.append({**row, "why": f"'{a_sem}' is not in the vocabulary"})
        elif a_conf != "high" or b_conf != "high":
            escalated.append({**row, "why": "an arm declined to be confident"})
        else:
            conflict = unit_family_conflict(a_sem, ev["unit"])
            if conflict:
                escalated.append({**row, "why": f"unit family: {conflict}"})
            else:
                accepted.append({**row, "semantic": a_sem})

    # gate 4, once the per-row gates have run
    seen = {}
    for r in accepted:
        seen.setdefault(r["semantic"], []).append(r)
    keep = []
    for semantic, rs in seen.items():
        if len(rs) == 1:
            keep.append(rs[0])
        else:
            names = ", ".join(repr(r["name"]) for r in rs)
            for r in rs:
                escalated.append({**{k: v for k, v in r.items() if k != "semantic"},
                                  "why": f"{len(rs)} controls claim {semantic}: {names}"})
    return keep, escalated, declined


# --------------------------------------------------------------------------

def proposal_document(map_doc, accepted, escalated, declined, run_id, errors, effort):
    """The EXISTING proposals/<fp>.json shape, extended.

    `category` and `params[{index,kind,confidence,reason}]` are what
    ProposalSet::load already reads; unknown fields are ignored by it, so the
    provenance rides along without a schema change on the C++ side.
    """
    params = []
    for r in accepted:
        params.append({
            "index": r["index"],
            "kind": r["semantic"],
            "confidence": "high",
            "reason": f"both arms agree and are confident at effort {effort}; unit family holds",
            # -- what the answer RESTS ON, not just how it was reached --
            "semantic_source": "model-proposed",
            "trust": "llm-classified",
            "control_name": r["name"],
            "arms": r["arms"],
            "evidence": r["evidence"],
        })
    return {
        "schema": TOOL_VERSION,
        "fp": map_doc.get("fp"),
        "category": map_doc.get("category"),
        "plugin": map_doc["identity"]["name"],
        "run": {
            "run_id": run_id,
            "at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
            "models": [OPUS, GPT],
            "prompt_sha": SYSTEM_SHA,
            "effort": effort,
            "tool": TOOL_VERSION,
        },
        "params": params,
        "escalations": escalated,
        "declines": [{"index": d["index"], "control_name": d["name"],
                      "reason": d["why"], "arms": d["arms"]} for d in declined],
        "errors": errors,
    }


def run_one(map_path, out_dir, clients, audit, run_id, effort):
    map_doc = json.load(open(map_path))
    rows = candidates(map_doc, audit=audit)
    name = map_doc["identity"]["name"]
    if not rows:
        return name, 0, 0, "nothing to propose"

    user = format_for_prompt(map_doc, rows)
    (a_ans, a_err) = with_retry(ask_opus, clients["anthropic"], user, effort)
    (b_ans, b_err) = with_retry(ask_gpt, clients["openai"], user)
    errors = [e for e in (a_err, b_err) if e]
    if a_ans is None or b_ans is None:
        # ONE ANSWER IS NOT AN AGREEMENT, so nothing can be accepted here. Write
        # NO file: presence of a proposal file is what "already done" means, so
        # recording a transient 529 as a result would permanently poison this
        # plugin until someone noticed and passed --force. Leave it for the next
        # run instead.
        return name, 0, 0, f"ARM FAILED, left for the next run: {errors[0][:60]}"

    accepted, escalated, declined = merge(rows, a_ans, b_ans)

    if audit:
        truth = {r["name"]: r.get("truth") for r in rows}
        hit = sum(1 for r in accepted if truth.get(r["name"]) == r["semantic"])
        AUDIT.append({"plugin": name, "accepted": len(accepted), "correct": hit,
                      "escalated": len(escalated), "declined": len(declined),
                      "total": len(rows),
                      "wrong": [{"control": r["name"], "proposed": r["semantic"],
                                 "human": truth.get(r["name"]),
                                 "unit": r["evidence"]["unit"], "arms": r["arms"]}
                                for r in accepted if truth.get(r["name"]) != r["semantic"]]})
        return name, len(accepted), len(escalated), f"audit {hit}/{len(accepted)} match human"

    doc = proposal_document(map_doc, accepted, escalated, declined, run_id, errors, effort)
    json.dump(doc, open(os.path.join(out_dir, f"{map_doc['fp']}.json"), "w"), indent=1)
    return name, len(accepted), len(escalated), f"{len(declined)} declined"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--only", default=None, help="substring match on plugin name")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=3)
    # The hold-out measured 98.7% auto-accept precision at effort HIGH, so that
    # is the default and anything else is a different configuration. It is a
    # flag rather than a constant because high is periodically 529-Overloaded
    # for capacity reasons, and it is recorded on every row it produces so a
    # proposal can never be mistaken for one made at a different setting.
    ap.add_argument("--effort", default="high",
                    choices=["low", "medium", "high", "xhigh", "max"])
    ap.add_argument("--force", action="store_true", help="redo maps already proposed")
    ap.add_argument("--audit", action="store_true",
                    help="re-derive already-confirmed params and score against them")
    args = ap.parse_args()

    import anthropic, openai
    clients = {"anthropic": anthropic.Anthropic(), "openai": openai.OpenAI()}

    maps_dir = os.path.join(args.root, "maps")
    out_dir = os.path.join(args.root, "proposals")
    os.makedirs(out_dir, exist_ok=True)

    paths = sorted(os.path.join(maps_dir, f)
                   for f in os.listdir(maps_dir) if f.endswith(".json"))
    todo = []
    for p in paths:
        d = json.load(open(p))
        if args.only and args.only.lower() not in d["identity"]["name"].lower():
            continue
        if not args.audit and not args.force \
           and os.path.exists(os.path.join(out_dir, f"{d['fp']}.json")):
            continue
        todo.append(p)
    if args.limit:
        todo = todo[:args.limit]

    run_id = datetime.datetime.now().strftime("%Y%m%dT%H%M%S")
    print(f"{len(paths)} maps, {len(todo)} to propose  "
          f"[{OPUS} @ effort {args.effort} + {GPT}, prompt {SYSTEM_SHA}]"
          + ("  AUDIT MODE -- writes nothing" if args.audit else ""))
    if not todo:
        return

    t0, acc, esc = time.time(), 0, 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        futures = {ex.submit(run_one, p, out_dir, clients, args.audit, run_id, args.effort): p
                   for p in todo}
        for fut in concurrent.futures.as_completed(futures):
            try:
                name, a, e, note = fut.result()
            except Exception as exc:                            # noqa: BLE001
                print(f"  !! {os.path.basename(futures[fut])}: {exc}")
                continue
            acc += a; esc += e
            print(f"  {a:3d} accepted  {e:3d} to review   {name}"
                  + (f"   [{note}]" if note else ""), flush=True)

    if args.audit:
        n = sum(a["total"] for a in AUDIT); a_ = sum(a["accepted"] for a in AUDIT)
        c = sum(a["correct"] for a in AUDIT); e = sum(a["escalated"] for a in AUDIT)
        d = sum(a["declined"] for a in AUDIT)
        print(f"\nAUDIT over {n} params a human already confirmed, at effort {args.effort}")
        print(f"  auto-accepted   {a_}/{n} = {100*a_/n:.1f}%")
        print(f"    of those, matching the human answer   {c}/{a_} = {100*c/a_:.1f}%" if a_ else "")
        print(f"  escalated       {e}/{n} = {100*e/n:.1f}%")
        print(f"  declined 'none' {d}/{n} = {100*d/n:.1f}%")
        wrong = [w for a in AUDIT for w in a["wrong"]]
        print(f"\n  every accepted row that disagrees with your answer ({len(wrong)}):")
        for w in wrong:
            print(f"    {w['control']!r}: proposed {w['proposed']}, you said {w['human']}"
                  f"   unit {w['unit'] or '(none)'}")
        json.dump(AUDIT, open(os.path.join(args.root, f"audit-{run_id}.json"), "w"), indent=1)
        return

    total = acc + esc
    print(f"\n{acc} accepted, {esc} to review, of {total} controls "
          f"({100*acc/total:.0f}% written without you)" if total else "\nnothing proposed")
    print(f"{time.time()-t0:.0f}s   ->  {out_dir}")


if __name__ == "__main__":
    main()
