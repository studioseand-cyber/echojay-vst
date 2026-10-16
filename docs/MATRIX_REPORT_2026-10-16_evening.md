# The scenario matrix, 16 October 2026 (evening)

**What this is.** Sean's instruction this afternoon was to stop testing by hand and work to
`docs/AGENT_SCENARIO_MATRIX.md`: a runner that loads real plugins, asserts on what the editor draws,
keeps evidence, and runs both a deterministic (mock) pass and a JOINT LIVE pass against B's local
server with the real model. This is where that stands at the end of the day.

---

## 1. In one line

The deterministic matrix is **106 passed / 2 failed / 7 skipped** and the two failures are rows I had
just rewritten, not product faults (both are fixed and re-running). The **live half proved eight of
its thirteen rows at five attempts out of five**, including the two that mattered most this morning -
and then every model call on this account started failing, which is why the gate is INCOMPLETE and
**nothing is installed**.

---

## 2. The live half, and what it proved before the upstream went

| row | what it holds | result |
|---|---|---|
| **A3-live** | own rack, third party added with a setting: dialled AND confirmed | **5 of 5** |
| **A4-live** | the same on a **Link** - your morning item 5 | **5 of 5** |
| **B2-live** | tapping a chip answers the question and the run carries on | **5 of 5** |
| **C1-live** | the closing never claims a figure the rack does not show | **5 of 5** |
| **B3-live** | a typed answer lands in the waiting ask (earlier pass) | **5 of 5** |
| **C3-live** | "confirmed" only when a value was read back (earlier pass) | **5 of 5** |
| **E1-live** | a GR answer gives the loudest hits and the typical (earlier pass) | **5 of 5** |
| **E3-live** | an uncertified plugin is refused without blaming silence (earlier pass) | **5 of 5** |
| **B1-live** | a goal naming a track is answered ON that track | 4 of 5, row fixed |
| **A8/A9-live** | what the server offers when you name a plugin | SKIP: the model never asks |
| **C2-live** | a placed add states the position the rack has | blocked by the upstream |
| **B4-live** | a new instruction closes the waiting run | retargeted; see §4 |

**Five real faults came out of the live seam today**, every one of them invisible to the mock:

1. **The Link read-back** (your item 5). Two attempts in five said *"the settings did NOT take:
   Threshold is 0.00 (asked -18.00)"* four seconds into a round trip that runs V2 -> server -> V2 ->
   Link -> dial -> sidecar. The dial now says when it has finished (`dialPending`, additive in the
   sidecar) and an unread value is reported as not confirmed, never as a refused write. **A4-live is
   5 of 5 now.**
2. **The wrong copy.** With four Renaissance Axx on one Link rack the read-back named the *previous*
   attempt's value. The placement decides now, and where nothing can decide it says so and names no
   figure at all.
3. **The swallowed instruction** (your #21). "Set the pre-gain to 2 dB", typed into a chip-less ask,
   was taken as the ANSWER five times out of five. An answer is a name; an instruction is a sentence.
4. **"Untitled" racks.** Every Link in the rig had no track name, so "make the kick punchier" went to
   the mix bus. With real names the model asks *"There are two kick tracks linked - which one do you
   mean?"*
5. **The server's own ids on a step line** - `attack_ms 20, release_ms 60` - caught by invariant I4.

---

## 3. The evening's stall, and what it actually was

For about two hours every model call failed and the live half could not run:

- 3211 answered `/local/health` with `ok=true, agentOn=true` all evening while **every round** came
  back `round_failed "The agent couldn't start, please try again."`
- 3213 served rounds perfectly for twenty minutes and then began failing identically.
- Plain chat failed too: a well-formed `/api/chat-stream` body answered `{"error":"Internal server
  error"}`.

I wrote here that the fault was "upstream of this tree (the account's model access)". **It was not,
and B found it:** Node's built-in fetch kept dispatching on a pooled HTTP/2 session to the provider
that had been destroyed - `ERR_HTTP2_INVALID_SESSION` in both server logs, with no status code and
nothing from the provider at all (no 401, 402, 429 or 529). A restart cured it; B has added a local
watchdog that restarts a server whose `modelFetchFailures` counter grows, and that counter in the
health line is the tell if it ever happens again (FOR_A §52).

What the stall did produce is worth keeping: both pre-flights now send a REAL round and read what
comes back, because `/local/health` said ok through the whole thing. A gate that had trusted health
would have run an hour of rows against a server that could not answer and reported it as a product
failure.

**The gate resumed as soon as the model did** - it was at S4 when B restarted 3211, so its S1-S4
verdicts are about this same source and its S6 ran live. Nothing was installed while S6 could not
run, which is your rule.

---

## 4. Things the matrix cannot judge, and where they went instead

- **A8/A9** (what the server offers when you name a plugin it does not have / does have): the model
  never asks, so there is no offer for a machine to read. On the Logic smoke test as 9a.
- **B2's chips**: the server sends `{"question":"Pick a transient shaper...","choices":[],
  "allowFreeText":false}` five times in five - nothing to tap. FOR_B §15, with the object verbatim;
  the smoke test asks what YOUR installed set gets (9b).
- **T3 / T2w** (the rack-selection lock, and a Link's own window open): honest SKIPs, on the smoke
  test as step 9.
- **A11b** (Auto on another track): the matrix line expects a wait; the build decides on undoability
  alone, which is your own 14 Oct ruling. NEEDS_SEAN **D13** - a ruling for you.
- **D4** (a step's Undo): the history coalesces consecutive edits, so the button is offered only on a
  one-change run. NEEDS_SEAN **D17** - a button on every step needs a redo-aware history, your call.

---

## 5. What is in the build that is waiting

Twenty-two items from your morning recheck, plus five faults found by the runner today, plus the
runner itself: real third-party plugins, two Links in their own processes with real track names,
evidence PNGs per scenario, and the invariants (I4's wording check caught two things the rows
themselves would have passed). Nothing is installed; `ship_2026-10-16a` from 05:05 is still what you
are running.
