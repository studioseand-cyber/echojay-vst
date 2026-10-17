#!/usr/bin/env python3
"""
FIX UP THE EXTRACTED FIXTURES SO THEY CAN BE SERVED (17 Oct 2026).

Two things the log cannot give the extractor on its own:

1. An ask card's question and choices are logged on their own line ("EJAgent: ask card: ..."), not
   inside the `talk` tool_call that produced them. They are merged back into that call's args here,
   which is where the server really put them.

2. The TEXT of `message` frames was not logged by the build that ran this morning (it is now). Two of
   Sean's items are about those sentences, so the two he QUOTED are filled in here, each marked with
   where it came from - his report - so nobody later mistakes a quotation for a recording. Every other
   message frame stays empty, which is honest: I do not know what it said.
"""
import json, os, glob

D = os.path.dirname(os.path.abspath(__file__))

# Sean's own words this morning, against the session they belong to.
QUOTED = {
  # item 4: the bubble that said "Added" while the add was still waiting for Apply, naming the WRONG track.
  # His words: 'targeting 88_7even Kick_2_01, the run showed "Added Renaissance Axx to the Mix Bus..."
  # while the add still waited for Apply'.
  ("rp_cp_2", 2, 0): ("Added Renaissance Axx to the Mix Bus with the threshold set to -20 dB.",
                      "QUOTED BY SEAN, 17 Oct - the premature 'Added' bubble, naming the Mix Bus "
                      "while the target was 88_7even Kick_2_01 and the step still awaited Apply"),
  # item 6: the bubble repeating the closing when it has extra text before it.
  ("rp_cp_4", 6, 0): ("Done - I've set it up on the kick.\nI added a Tube-Tech CL 1B compressor to "
                      "the kick with threshold around -15 dB, ratio 4:1, attack 3 and release 5.",
                      "QUOTED BY SEAN, 17 Oct - 'the bubble still repeats the closing when the bubble "
                      "has extra text before it'; the second sentence is the done summary verbatim"),
}

for path in sorted(glob.glob(os.path.join(D, "rp_cp_*.json"))):
    d = json.load(open(path))
    sid = d["id"]
    # drop the empty leading round the extractor opens before the first `round` frame
    d["rounds"] = [r for r in d["rounds"] if r["frames"]]
    for r in d["rounds"]:
        merged = []
        for fr in r["frames"]:
            if "_ask" in fr:
                for prev in reversed(merged):
                    if prev.get("event") == "tool_call" and prev.get("name") == "talk":
                        prev.setdefault("args", {})["ask"] = fr["_ask"]
                        break
                continue
            merged.append(fr)
        r["frames"] = merged
    # ---- THE QUOTE GOES WHERE THE DEFECT HAPPENED, FOUND BY SHAPE (17 Oct 2026) --------------
    # Keyed by round number, the first cut put the "Added ... to the Mix Bus" sentence on the LAST
    # response - so the bubble appeared after the await instead of during it, and RP4 passed on a
    # build that draws it early. The round numbers move every time the grouping is corrected; the
    # shape does not:
    #   * the premature "Added ..." bubble belongs in the response that WAITS (it has a plan and an
    #     await), because that is the whole claim - it said "added" while the add was waiting;
    #   * the duplicate-closing bubble belongs in the response that FINISHES (it has the done
    #     frame), because the closing it repeats is that frame's summary.
    # IDEMPOTENT, because this script is run again every time the extractor improves. A quote left
    # from a previous run would pin the placement that run chose - which is exactly how the "Added"
    # sentence stayed on the done response after the placement rule was fixed.
    for r in d["rounds"]:
        for fr in r["frames"]:
            if fr.pop("textSource", None) is not None: fr["text"] = ""
    want = {k[0]: v for k, v in QUOTED.items()}
    if sid in want:
        text, source = want[sid]
        wantsWait = "Added" in text
        for r in d["rounds"]:
            kinds = [f.get("event") for f in r["frames"]]
            fits = ("await" in kinds and "plan" in kinds) if wantsWait else ("done" in kinds)
            if not fits: continue
            for fr in r["frames"]:
                if fr.get("event") == "message" and not fr.get("text"):
                    fr["text"], fr["textSource"] = text, source
                    break
            break
    # the tool args the executor needs, rebuilt from the call's own summary
    for r in d["rounds"]:
        for fr in r["frames"]:
            if fr.get("event") != "tool_call": continue
            s = fr.get("summary", "")
            if fr.get("name") == "do" and s.lower().startswith("add "):
                name = s[4:].split(" - ")[0].strip()
                fr.setdefault("args", {}).update({"op": "add", "name": name})
            elif fr.get("name") == "do" and "pre-gain" in s.lower():
                fr.setdefault("args", {}).update({"op": "set_pre_gain", "db": 2})
            elif fr.get("name") == "do" and s.lower().startswith("build "):
                fr.setdefault("args", {}).update({"op": "build", "summary": s})
            elif fr.get("name") == "look":
                fr.setdefault("args", {}).update({"what": "inventory" if "inventor" in s else "rack"})
    # ---- A PLAN FRAME WITHOUT ITS ITEMS IS NOT A PLAN FRAME (17 Oct 2026) --------------------
    # The log records `frame plan round=5 items=1 heading="The plan"` - the item COUNT, not the
    # array - so the first replay served a plan frame with no items, the client's item loop never
    # ran, and RP2 PASSED on a build that has the defect. The one frame the whole row is about was
    # the one frame I served empty.
    #
    # The items are recoverable without inventing anything: contract 3 says the plan lists the
    # round's ask-first items, and the round's tool_calls are in the log with their ids, summaries
    # and approvals. So the array is rebuilt from them, newest-last, trimmed to the logged count,
    # and marked `itemsSource` so nobody mistakes a reconstruction for a recording.
    for r in d["rounds"]:
        calls = [f for f in r["frames"] if f.get("event") == "tool_call"]
        for fr in r["frames"]:
            if fr.get("event") != "plan": continue
            it = fr.get("items")
            want = len(it) if isinstance(it, list) else int(it or 0)
            askers = [c for c in calls if (c.get("approval") or "ask_first") == "ask_first"]
            pick = (askers or calls)[:want] if want else (askers or calls)
            fr["items"] = [{"id": c.get("id"), "summary": c.get("summary", ""),
                            "why": "", "approval": c.get("approval") or "ask_first"} for c in pick]
            fr["itemsSource"] = ("rebuilt from this response's tool_calls - the log records the plan "
                                 "frame's item COUNT (%d), not the array" % want)
    json.dump(d, open(path, "w"), indent=1)
    msgs = sum(1 for r in d["rounds"] for f in r["frames"] if f.get("event") == "message")
    quoted = sum(1 for r in d["rounds"] for f in r["frames"] if f.get("textSource"))
    print("%-9s rounds=%-2d frames=%-3d message=%-2d quoted=%d" %
          (sid, len(d["rounds"]), sum(len(r["frames"]) for r in d["rounds"]), msgs, quoted))
