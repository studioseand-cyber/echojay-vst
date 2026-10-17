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
    # the quoted prose, with its provenance attached to the frame itself
    msg_seen = {}
    for r in d["rounds"]:
        for fr in r["frames"]:
            if fr.get("event") == "message":
                n = msg_seen.get(r["round"], 0); msg_seen[r["round"]] = n + 1
                key = (sid, r["round"], n)
                if key in QUOTED and not fr.get("text"):
                    fr["text"], fr["textSource"] = QUOTED[key]
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
    json.dump(d, open(path, "w"), indent=1)
    msgs = sum(1 for r in d["rounds"] for f in r["frames"] if f.get("event") == "message")
    quoted = sum(1 for r in d["rounds"] for f in r["frames"] if f.get("textSource"))
    print("%-9s rounds=%-2d frames=%-3d message=%-2d quoted=%d" %
          (sid, len(d["rounds"]), sum(len(r["frames"]) for r in d["rounds"]), msgs, quoted))
