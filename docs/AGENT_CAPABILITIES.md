# What the agent must be able to do — 14 Oct 2026

Sean's ask (13 Oct): *"use the host rig to test how much the agent can actually do, fix every path it can't,
and repeat until it can do everything."* This is the list the rig runs. **A, draft 1 — B to review and add
the server-side rules it owns** (classification, planning, the words the model is told to use).

## How to read a row

- **Request** is what a user types, verbatim. Not a tool call: if the agent needs a different tool than the
  one it picks, that is the capability failing, not the test being unfair.
- **Pass rule** is checked against **real state**, never against the agent's own sentence. The rig reads the
  rack back from the plugin or from the Link's own sidecar, reads the rack OUT value, reads the transcript
  the processor holds. *"It said it did"* is never a pass — that is precisely the fault that shipped three
  rounds running.
- **Owner** is who fixes it when it fails: **A** (plugin), **B** (server/model), or **both**.

A row passes only when it passes **with the agent chip ON** and leaves the rack in the state the user asked
for. A row that cannot be attempted (a missing plugin, no audio) is **blocked**, not passed.

---

## 1. Reading

| # | Request | Pass rule (real state) | Owner |
|---|---|---|---|
| R1 | "what's on this rack?" | every slot name the agent lists matches `ChainHost` slot-for-slot, in order, and it names no slot that is not there | A |
| R2 | "what's on the kick?" (another track's Link) | the names match that **Link's own sidecar**, read by the rig from the Link, not from the V2's view of it | A |
| R3 | "what compressors do I have?" | the names it offers are all in the installed list, and it covers the list — `total` is reported and paging is used when `hasMore` is true | both |
| R4 | "what EQs do I have?" | as R3 | both |
| R5 | "how loud is this?" | the figure matches `look(analysis)`'s integrated LUFS within 0.5 LU, and it says so in LUFS | A |
| R6 | "how much is the compressor doing?" (open rack) | the GR figure matches `check(gr)` within 0.5 dB | A |
| R7 | the same, aimed at another track's Link | **refuses plainly** with `not_available_on_link` wording, and never says "slot not on rack" | A |

## 2. One change on the OPEN rack

| # | Request | Pass rule (real state) | Owner |
|---|---|---|---|
| C1 | "add a compressor" | a slot appears on the rack, read back; its name is a plugin that exists; the agent's claim and the rack agree | A |
| C2 | "add FabFilter Pro-C 2 after slot 1" | the new slot is at position 2, read back | A |
| C3 | "remove slot 3" | the rack holds one fewer slot and the named one is gone | A |
| C4 | "move the de-esser before the compressor" | the order read back matches | A |
| C5 | "bypass slot 2" | that slot reads bypassed. **Was failing before 14 Oct**: the executor read `on`, the contract sends `bypassed` | A |
| C6 | "set the compressor's threshold to -18" | the slot's own parameter reads -18 in the device's units | both |
| C7 | "make the reverb 30% wet" | the slot's wet reads 0.30. **Was failing before 14 Oct**: `pct` vs `wet_pct` | A |
| C8 | "turn the rack output down 2 dB" | the rack OUT reads 2 dB below where it was — the rack's OUT, **not** a plugin's output_db | A |
| C9 | "add 3 dB of pre gain" | the pre-gain reads +3 dB | A |
| C10 | "undo that" | the rack returns to its previous state, read back. **Sean's ruling 4, 14 Oct**: an undo walks the plugin's one undo history ONE STEP, whatever that step was — the model does not choose what "that" meant. `{"op":"undo"}`, no arguments. The op reports success only when the history's depth actually moved; an empty history answers "there was nothing to undo"; a step whose target has gone is reported skipped with its reason and is never applied to a different target | A |

## 3. The same on ANOTHER track's Link

| # | Request | Pass rule (real state) | Owner |
|---|---|---|---|
| L1 | "add a compressor on the kick" | the slot appears on **that Link's** rack, read from the Link | A |
| L2 | "build a chain on the kick: EQ then compressor" | both slots on that Link, in order, read from the Link | A |
| L3 | "turn the vocal's output down 2 dB" | that Link's OUT gain reads 2 dB lower, confirmed from the registry the Link publishes | A |
| L4 | "bypass slot 1 on the kick" | read back from the Link | A |
| L5 | a request naming a track that does not exist | refuses, names the channels that do exist, and changes nothing | A |
| L6 | a request while that Link is offline | refuses plainly; nothing is claimed | A |

## 4. Multi-step goals

These are the rows that decide whether the agent is useful rather than merely correct.

| # | Request | Pass rule (real state) | Owner |
|---|---|---|---|
| G1 | "make the kick punchier" | the rack changes; every slot it added exists; it says what it did and the rack agrees | both |
| G2 | "de-ess the vocal" | a de-esser (or an EQ doing that job) is on the vocal's rack, read back | both |
| G3 | "glue the mix bus, about 2 dB" | a bus compressor on the rack and GR measures 1.5–3 dB with audio playing | both |
| G4 | "bring the vocal up to sit with the beat" | the vocal's measured level moves toward the reference and the agent states both figures | both |
| G5 | "build me a vocal chain" | the rack holds the chain it described, slot for slot; **and the rack is no louder than its input** (the post-build match landed) | A |
| G6 | a goal needing a plugin that is not installed | it says so, names what it would use instead, and does not pretend | both |

## 5. Approvals

| # | Request | Pass rule (real state) | Owner |
|---|---|---|---|
| A1 | a one-line plan, press **Apply** | the line runs and is recorded applied — **not** "skipped" (the 13a fault) | A |
| A2 | a multi-line plan, press **Apply all** | every line runs, in order | A |
| A3 | a multi-line plan, skip one line then Apply | the skipped line is recorded skipped and did not touch the rack; the others ran | A |
| A4 | press **Skip all** | nothing reached the rack; the rack is byte-identical | A |
| A5 | a change on another track | **asked once**, and pressing Apply on the plan is the whole confirmation — the executor must not ask again | A |
| A6 | agent mode OFF | the agent proposes and changes nothing; the rack is untouched | A |

## 6. Failure and recovery

| # | Request | Pass rule (real state) | Owner |
|---|---|---|---|
| F1 | a plugin that fails to load | the agent is told, says so, and the rack is left usable | A |
| F2 | an op this build does not have | refused with the op NAMED and the ops it does have listed | A |
| F3 | the same failure twice | it stops rather than looping (the 18-round failure must not recur) | both |
| F4 | press **Stop** mid-run | no further step touches the rack, and the card stays up so Undo all is reachable | A |
| F5 | **close the window mid-run** | the user's message and whatever finished are in the transcript on reopen; nothing half-applies | A |
| F6 | "undo everything you just did" | the rack returns to its pre-run state, read back | A |

## 7. Honesty — the rules that outrank being useful

| # | Rule | How the rig checks it | Owner |
|---|---|---|---|
| H1 | never claims a change that did not land | every claim is checked against a read-back; one false claim fails the row | A |
| H2 | never names a plugin that is not on the rack | the names in its reply are intersected with the real slot list | both |
| H3 | says plainly when it cannot measure | the Link-GR case returns the `not_available_on_link` sentence, with `retryable:false` | A |
| H4 | a refusal reaches the user as ONE plain line | the user-facing line carries no uid, no imperative, no channel dump | A |
| H5 | never claims to have heard audio it has not | with the transport stopped it says so rather than reporting a figure | A |

---

## What the rig proves, and what it cannot

**Proves**: everything above, against the real built bundles, with audio running and editors opening and
closing on a script.

**Cannot prove**: Logic's own window parenting and automation, the AU hosting service's lifecycle, and
anything about a licence-bound plugin (never loaded — Sean's rule). Those stay Sean's pass.

## The loop

1. Run the rig over this list.
2. Write `docs/AGENT_SCORECARD.md`: pass / fail / blocked per row, the cause, and the owner.
3. A's failures get fixed; B's go to B in a FOR_B note; anything that changes behaviour goes to Sean first.
4. Re-run. Repeat until every row passes or is blocked on a decision.
5. Once the scorecard is green: one full gate from S1 including S6, then place, install, verify UUIDs.
