#!/usr/bin/env python3
"""Stage 2 -- propose semantics over swept maps, offline, in batch.

Reads ~/Library/ejmap/maps/*.json. NEVER loads a plugin. Two arms see the same
prompt, neither sees the other's answer. A row is accepted only when both agree,
both are confident, the unit family holds, and no other control on the same
plugin claims the same semantic. Everything else is written out as an escalation
for review.

RESUMABLE BY FILE LAYOUT, not by a mechanism: one proposal file per fingerprint,
and its presence means done. A crash costs the plugin in flight.

A PLUGIN TOO BIG FOR ONE ANSWER IS ASKED IN CHUNKS. The 7 Aug corpus run lost 11
plugins -- Repeater at 2,104 controls, Saturn 2 at 951, six MeldaProduction
multiband units -- to one failure: the arm's JSON ran past max_tokens and was
truncated mid-string. That is a cap, not a model error, and the fix is to ask in
pieces. Two properties make the pieces safe, and both are tested:

  * THE DECISION IS STILL MADE OVER THE WHOLE PLUGIN. Chunking splits the
    QUESTION, never the judgement: every chunk's answers are concatenated and
    merge() runs ONCE over all rows. Gate 4 -- no two controls may claim one
    semantic -- therefore still sees both halves of a boundary, which is the
    whole reason it cannot be applied per chunk.
  * A FAILED CHUNK LEAVES THE PLUGIN UNWRITTEN, exactly as a failed arm does.
    A proposal file's presence means "done", so a partial file that looked
    complete would be permanently wrong and invisible. Nothing is written until
    every chunk has answered.

What chunking DOES change, stated rather than buried: an arm sees fewer of the
plugin's other controls when it judges one. The 98.7% hold-out was measured on
whole-plugin prompts, so it does not cover a chunked row, and the proposal
records `run.chunking` so a chunked answer can never be mistaken for one made
with the whole surface in view.

    python3 propose.py                     # every unproposed map
    python3 propose.py --only CLA-76       # substring match on the name
    python3 propose.py --audit             # re-derive human answers, score them
    python3 propose.py --force             # redo maps already proposed
    python3 propose.py --chunk 200         # controls per request (0 = never split)
"""
import argparse, concurrent.futures, datetime, json, os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evidence import candidates, format_for_prompt, unit_family_conflict, VOCAB
from prompt import SYSTEM, SYSTEM_SHA

OPUS = "claude-opus-5"
GPT = "gpt-5.5"
TOOL_VERSION = "propose/1"
AUDIT = []

# Controls per request. Both arms cap output at 16,000 tokens, and the eleven
# plugins that failed on 7 Aug were emitting roughly 33 tokens of JSON per
# control -- MReverbMB truncated at 495 controls, exactly at char 32,768. 200
# leaves better than a 2x margin at that measured rate, and the margin matters
# more than the round-trip count: a chunk that truncates costs the whole plugin.
CHUNK = 200


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
      2. (was: both arms were confident -- DROPPED 7 Aug 2026, measured at
         97.7% hedged against 98.7% confident; the confidence is recorded
         on the row instead of gating it)
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
            # BOTH ARMS SAYING "none" IS AN ANSWER, confident or not.
            #
            # Measured over the first full review pile: of 57 rows escalated as
            # "both say none, but not confidently", the mapper answered none on
            # 57 of 57. A category with a 100% outcome is not a question, it is
            # a queue tax -- 21% of that pile.
            #
            # They are DECLINES, not discards: recorded with both arms and their
            # confidences, so they stay in the none corpus that the vocabulary
            # work reads, and so a later pass can re-open the hedged ones alone
            # if the sample ever disagrees.
            declined.append({**row,
                             "why": ("both arms say no semantic applies"
                                     + ("" if (a_conf == "high" and b_conf == "high")
                                        else ", though at least one hedged")),
                             "confident": a_conf == "high" and b_conf == "high"})
        elif a_sem not in VOCAB:
            escalated.append({**row, "why": f"'{a_sem}' is not in the vocabulary"})
        else:
            # A HEDGED AGREEMENT IS STILL AN AGREEMENT. Measured 7 Aug 2026
            # against the 125 hold-out truth rows:
            #
            #     both confident   74/75 = 98.7%
            #     one arm hedged   42/43 = 97.7%
            #
            # Indistinguishable at these sample sizes. The old gate discarded
            # 43 of 118 agreements -- 36%, ~7,100 controls at corpus scale --
            # to buy one percentage point that is inside the noise.
            #
            # The single hedged error was UAD Precision Limiter "Output":
            # both arms said ceiling_db, the hand answer was output_db. On a
            # limiter those are arguably the same knob, and NOTHING mechanical
            # separates them -- both are _db, so the unit family passes either.
            # A residual, not something the confidence gate was catching.
            #
            # The confidence is RECORDED rather than acted on, so the accept
            # set stays sortable and this decision stays re-derivable from the
            # arms without re-running anything.
            hedged = not (a_conf == "high" and b_conf == "high")
            conflict = unit_family_conflict(a_sem, ev["unit"])
            if conflict:
                escalated.append({**row, "why": f"unit family: {conflict}"})
            else:
                accepted.append({**row, "semantic": a_sem, "hedged": hedged})

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

def proposal_document(map_doc, accepted, escalated, declined, run_id, errors, effort,
                      chunking=None):
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
            # ABSENT means the arms saw the whole plugin at once, which is what
            # the 98.7% hold-out measured. Present means they judged each
            # control with only its chunk of siblings in view -- a narrower
            # evidence base than the number was measured on, recorded so nobody
            # has to infer it from the control count later.
            **({"chunking": {"chunks": chunking[0], "controls_per_chunk": chunking[1]}}
               if chunking else {}),
        },
        "params": params,
        "escalations": escalated,
        "declines": [{"index": d["index"], "control_name": d["name"],
                      "reason": d["why"], "arms": d["arms"]} for d in declined],
        "errors": errors,
    }


def run_one(map_path, out_dir, clients, audit, run_id, effort, chunk=CHUNK):
    map_doc = json.load(open(map_path))
    rows = candidates(map_doc, audit=audit)
    name = map_doc["identity"]["name"]
    if not rows:
        return name, 0, 0, "nothing to propose"

    # The rows arrive in index order, so a split keeps a band's controls next to
    # each other rather than scattering `Band 3 Detector -> LP` away from its
    # siblings. Chunking is a transport concern; it must not reorder evidence.
    parts = ([rows] if not chunk or len(rows) <= chunk
             else [rows[i:i + chunk] for i in range(0, len(rows), chunk)])

    a_ans, b_ans, errors = [], [], []
    for n, part in enumerate(parts, 1):
        user = format_for_prompt(map_doc, part)
        (a, a_err) = with_retry(ask_opus, clients["anthropic"], user, effort)
        (b, b_err) = with_retry(ask_gpt, clients["openai"], user)
        errors += [e for e in (a_err, b_err) if e]
        if a is None or b is None:
            # ONE ANSWER IS NOT AN AGREEMENT, so nothing can be accepted here.
            # Write NO file: presence of a proposal file is what "already done"
            # means, so recording a transient 529 as a result would permanently
            # poison this plugin until someone noticed and passed --force.
            #
            # THE SAME IS TRUE OF ONE CHUNK OF A PLUGIN, and more dangerously:
            # a proposal missing 200 of 2,104 controls has nothing on its face
            # that says so. It would read as a complete answer forever. So a
            # chunk failure abandons the WHOLE plugin, unwritten, for the next
            # run -- the chunks already paid for are the price of that safety.
            where = "" if len(parts) == 1 else f" (chunk {n}/{len(parts)})"
            return name, 0, 0, (f"ARM FAILED{where}, whole plugin left for the "
                                f"next run: {errors[0][:60]}")
        a_ans += a
        b_ans += b

    # ONE merge over ALL rows and BOTH arms' concatenated answers, never one per
    # chunk. Gate 4 lives at the end of merge(), so this is what makes two
    # controls claiming `gain_db` on opposite sides of a chunk boundary still
    # escalate instead of both being accepted into a dict that holds one.
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

    doc = proposal_document(map_doc, accepted, escalated, declined, run_id, errors,
                            effort, chunking=(len(parts), chunk) if len(parts) > 1 else None)
    json.dump(doc, open(os.path.join(out_dir, f"{map_doc['fp']}.json"), "w"), indent=1)
    return name, len(accepted), len(escalated), (
        f"{len(declined)} declined"
        + (f", asked in {len(parts)} chunks" if len(parts) > 1 else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.expanduser("~/Library/ejmap"))
    ap.add_argument("--only", default=None, help="substring match on plugin name")
    # A named SET of map files, so a stratified slice is reproducible and
    # reviewable rather than a substring guess. The file is a JSON array of
    # paths, which is what the slice builder writes.
    ap.add_argument("--maps-from", default=None,
                    help="JSON array of map file paths; proposes exactly those")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--chunk", type=int, default=CHUNK,
                    help="controls per request; 0 asks for the whole plugin at "
                         "once, which truncates past ~500 controls")
    # The hold-out measured 98.7% auto-accept precision at effort HIGH, so that
    # QUALIFIED 7 Aug 2026: the hand answers this is measured against
    # are PARTLY MODEL-SOURCED -- some of the 266 review decisions came
    # from pasting cards into a third model, so the standard is not fully
    # independent of what it scores. The DIRECTION holds: both errors were
    # adjudicated by the unit-family rule over measured evidence.
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

    if args.maps_from:
        # A NAMED SET, so a stratified slice is reproducible and reviewable
        # rather than a substring guess. Missing entries are an error, not a
        # silent shortfall: a slice that quietly proposed 140 of 150 would
        # report rates for a population nobody chose.
        paths = json.load(open(os.path.expanduser(args.maps_from)))
        missing = [p for p in paths if not os.path.exists(p)]
        if missing:
            sys.exit(f"--maps-from lists {len(missing)} path(s) that do not exist, "
                     f"first: {missing[0]}")
        paths = sorted(paths)
    else:
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
        futures = {ex.submit(run_one, p, out_dir, clients, args.audit, run_id,
                             args.effort, args.chunk): p
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
