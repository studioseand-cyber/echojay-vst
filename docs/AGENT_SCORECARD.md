# Agent scorecard — 14 Oct 2026 (loops 1–3)

Run by `tools/host_rig/` against **B's local server** (a second instance on 127.0.0.1:3211, started from B's
own tree with `FORGE_DEV_OPEN=1` so the dev login answers; B's instance on 3210 untouched, their tree
unedited), the **real model**, the **real built bundles** (V2 + two Links in one host process), audio playing,
editors opening and closing on a script. Every row asserts on **real state** — the rack read back from the
plugin or from the Link's own sidecar, the rack OUT, the Link's published gain, the transcript the processor
holds. Never on what the agent said it did.

## The vacuous-pass gate (14 Oct, Sean's instruction) - proven both ways

**Every capability row now requires its turn to have actually run.** `AgentOutcome::ran()` is true when the
card **adopted this row's goal** and produced a step, an answered question, or visible text. A refusal
counts - F2 and L5 are *meant* to be refused, and a refusal is a turn that ran and said no. Silence does not.
`row()` takes the outcome and cannot pass without one; it prints
`FORCED FAIL: the assertion held but NO TURN RAN (<why>)`.

| direction | run | result |
|---|---|---|
| RED (`EJ_RIG_BREAK_AGENT=1`, every agent POST refused) | 15:01 | **no capability row passes.** C10, L5, H3, F2 - the four that passed vacuously at 14:34 - all FORCED FAIL |
| GREEN (a real token, the installed ship) | 15:05 | **41 passed, 0 failed, 0 skipped.** The gate costs no legitimate row |

### It took three red runs, and each one found a different hole

1. the four vacuous passes, caught by `ran()`.
2. **C5 and C7 vanished** - guarded on the rack having slots, so with every turn failing they printed
   nothing at all: fifteen rows where seventeen were expected, and no line saying which two were missing.
   Both now SKIP loudly. Same defect as this morning's `ok (skipped)`, in its other form.
3. **A6 passed twice more.** It runs no agent turn by design (the chip is off), so its evidence had to be a
   chat reply - and a transcript row count counted the **401 error bubble** as one. Then the blacklist missed
   the actual copy (*"Session expired. Please log in again."* - the list had "sign in", not "log in").
   Matching failure strings is the wrong polarity: the copy changes and the row goes quietly vacuous again.
   It now requires **positive** evidence - the reply must name the **limiter** it asked for, which no
   session-expired notice ever will.

**`EJ_RIG_BREAK_AGENT=1` stays in the rig.** A gate against vacuous passes that cannot itself be driven red
is unfalsifiable, which is the thing it exists to prevent.

### L2 is flaky, and now measurable: `EJ_RIG_L2_REPEAT=N`

Four observations on the same binary: 12:34 **pass**, 14:01 fail (`check(level_landed)`), 14:42 fail (a
round that produced nothing), 15:05 **pass**. Rows are named `L2#1..L2#N`, and **every attempt after the
first must show a build reaching that Link in its own steps** - on a repeat the rack is already built, so
"it holds two slots" would be the vacuous pass in its other form.
## Loop 6 (14 Oct, 14:42) - S6 live against the INSTALLED ship_2026-10-14a: **16 passed / 1 failed**

The shipped binary, hosted from `~/Library` (arm64 `07FD2B0D...` / `402E05C9...`), against B's tree after
their run 14. **No plugin change since the gate.**

Every row passes except **L2** (build a chain on a Link). C10 passes again. L2's cause is in the model's
second round: having read the Link's rack it sent no tool call, no text and an empty `done.summary`, so
nothing was built - owner B, with a sim row asked for in `docs/FOR_B_2026-10-14.md`, because L2 has now
passed once and failed twice for two different reasons on the same binary.

### A harness weakness this run exposed, not yet fixed

In the 14:34 run - the one spoiled by a server-side `500 route_load_failed` - **several rows passed
vacuously**: C10 "undo that" passed because the rack had not changed, the removal it was undoing having
itself failed, so before == after. A6, L5 and F2 can pass the same way: their assertions are variants of
"nothing was touched". **A row whose assertion is satisfied by the turn never running is not a test.**
`askAgent` already records whether the card adopted the goal and what the steps said, so the facts are
there; the rows must require them. Queued with the next round's rig work - the same family as the skip that
printed `ok` and padded a pass count for a whole round.
## Loop 5 (14 Oct, 14:01, as gate11's S6): **live 40/1, mock 25/0** - C10 GREEN, L2 red and it is B's

| row | verdict | note |
|---|---|---|
| C10 | **PASS** | `{"op":"undo"}` end to end through the real model: the capability declared, B's schema and UNDO paragraph live after the restart, the label reported from the history. **Sean's ruling 4, done.** |
| U1 (mock) | **PASS** | the same op driven as scripted SSE frames with the model taken out - and the leg that found the undo snapshot's missing rack-level values |
| L2 | **FAIL** | the model called `check(level_landed)`, which this build does not have, and spent the turn on it - so the build never happened and that Link's sidecar still held L1's compressor |

**L2's cause is B's tool schema, not the plugin.** `api/_agent-loop.js:171` correctly tells the model
*"LINK METERS: not in this build"* when `link_meters` is not declared, but `api/_agent-tools.js:45` carries
`level_landed` in the `check.what` enum **unconditionally**. The model reads the schema, calls it, and my
refusal names the six checks that exist.

**The same plugin passed L2 live at 12:34 today** (`ROW L2 PASS`, same code path, the chain read back from
the Link's own sidecar). B's server changed between the two runs. So the binary is proven on that row and
the red is in the server's schema - which is why the fix asked for in `docs/FOR_B_2026-10-14.md` is one gate
on their enum, the mirror of the capability I failed to declare this morning.

### Gate11: S1-S5 GREEN (77/77), S6 RED on that one row

Nothing else in the gate moved: the forced six-archive delete and relink across three trees, 87 guard
targets, the four bundles, archive provenance, 77/77 ctest, `link_state_guard`, and the mock rig pass are
all green. `downgrade_keys_guard` passes, so a project written by this build still opens in
`ship_2026-10-09a` - this round's new fields are in-memory (the undo snapshot) or on the wire (the
capability), not in saved state.
## Loop 4 (14 Oct, 11:36-11:41, S6 run live): **40 passed, 1 failed, 0 skipped**

| row | verdict | what changed |
|---|---|---|
| L3 | **PASS** | the turn-adoption fix held: a reply now belongs to a row only when the card adopted that row's goal |
| the slot double-click | **PASS** | deferred to after C1, where a slot exists - it was printing `ok (skipped)` and padding the count |
| C10 | **FAIL** | **and the cause was mine, not the model's** - see below |

### C10: the op was built, guarded, and unreachable

The agent answered *"Reverted the pre-gain change, it's back to 0 dB."* It ran `look`, then
`do set_pre_gain 0` - it never called `{"op":"undo"}`, exactly as before the op existed.

The reason is in B's contract, written this morning:

> *"The server sends it only when the start body's `capabilities` includes \"undo\"; otherwise it is refused
> in process (`undo_unavailable`)."*

`ExecutorRead::startContext` sends `capabilities: ["look","check","wait_for_playback","checkpoint"]`. **No
"undo".** So B's gate refused the op in process and the model, having no undo tool, chose what "that" meant -
which is the whole fault Sean's ruling 4 was meant to remove.

**I had read B's note, implemented the op, guarded it with sixteen assertions, written them a contract
describing it, and never sent the one word that makes it reachable.** The guard could not catch it because
the guard tests the executor, and the executor was right; the gap was between the plugin and the server, and
only a live run crosses that. It is the same lesson as the nine sinks in a new place: a thing is not done
because the half you can see is done.

**Fixed**: `ExecutorDo::startContext` appends `"undo"` to the capabilities array **when the sink exists** -
conditional, not hardcoded, because a capability is a claim about this binary and a build with no undo route
must not make it. Two new guard legs hold both directions.
## Loop 3 (14 Oct, 09:36–09:43, in the gate as S6): **39 passed, 2 failed**

Run as the gate's S6 stage, live against B's server on 3211. Two rows went red and they are different kinds
of red.

| row | request | verdict | cause | owner |
|---|---|---|---|---|
| C10 | "undo that" | **FAIL** | *"I can't see a history of the previous action in this session, so I'm not sure what \"undo\" refers to here."* The model had no undo tool to call. **Sean ruled on it this morning** and the op now exists (`{"op":"undo"}`, ruling 4) — this row is what the ruling was for | A, fixed |
| L3 | "turn &lt;Link&gt;'s output down 2 dB" | **FAIL** | **the rig's own fault.** The reply it captured was row L2's build summary. L2's turn ran 183 s against a 180 s bound with a **second plan still awaiting approval** — `askAgent` pressed Apply once and then watched it expire — so L3's request was sent into a session that was still executing | A, fixed |

L3 is worth stating plainly: the row was not wrong that the Link's OUT had not moved. **Nothing had asked it
to move.** A harness that reads one turn's answer as another's can report any fault it likes, including none.

### What the rig gained from it

- **it never sends into a live session**: `askAgent` waits for the card to be quiet and inactive first, up to
  90 s, and says so when it cannot get quiet.
- **a reply belongs to a row only when the card adopted that row's goal.** The card publishes the goal it is
  working on; waiting for it to BE this request turns "the state looks settled" into a fact about *which* turn
  settled — the one thing the old loop could not tell.
- **it approves every plan the card presents**, up to three, because a build can present a second one. A user
  presses Apply as often as asked.
- **it never hands the next row a live session**: a turn that misses its bound is waited out and reported.
- L2's bound is 300 s, not 180. The build really takes ~183 s against the real model with two approvals: the
  bound, not the build, was what failed, and a bound tighter than the work it measures manufactures faults.

## Loop 2: **40 passed, 1 failed** (15 capability rows + 25 lifecycle legs)

| row | request | verdict | evidence | owner |
|---|---|---|---|---|
| C1 | "add a compressor" | **PASS** | the open rack grew by one, read back | A |
| C2 | "add an EchoJay EQ after slot 1" | **PASS** | the new slot is at position 2 | A |
| C3 | "remove slot 1" | **PASS** | one fewer slot, read back | A |
| C5 | "bypass slot 1" | **PASS** | slot 1 reads bypassed | A |
| C7 | "make the compressor 30% wet" | **PASS** | a slot reads 0.30 wet | A |
| C8 | "turn the rack output down 2 dB" | **PASS** | the rack's own OUT moved −2 dB, not a plugin's output_db | A |
| C9 | "add 3 dB of pre gain" | **PASS** | the pre-gain reads +3 dB | A |
| C10 | "undo that" | **FAIL** | the agent answered *"I've undone the pre-gain change"* — it re-dialled an earlier parameter instead of undoing the removal. The plugin's undo history is ONE stack and the removal is on it, so the plugin could have undone it: the model chose otherwise | **B** |
| L1 | "add a compressor on &lt;Link&gt;" | **PASS** | that Link's **own sidecar** grew by one | A |
| L2 | "build a chain on &lt;Link&gt;: an EQ then a compressor" | **PASS** | that Link's sidecar holds both, in order — **Sean's item 1, end to end with the real model** | A |
| L3 | "turn &lt;Link&gt;'s output down 2 dB" | **PASS** | the Link's published OUT moved −2 dB | A |
| L5 | "add a compressor on the Trombone bus" (no such track) | **PASS** | it asked which track, and no rack we have was touched | A |
| R3 | "what compressors do I have?" | **PASS** | answered from the paged inventory | both |
| A6 | agent OFF: "add a limiter" | **PASS** | the rack is untouched; it proposed instead | A |
| F2 | "set the rack's tilt to 3" (no such op) | **PASS** | refused, stopped rather than looping | A |
| H2 | never names a plugin that is not on the rack | **PASS** | claims intersected with the real slot list | both |
| H3 | GR aimed at another track's Link | **PASS** | said plainly it cannot measure a Link yet; never "slot not on rack" | A |

**Lifecycle legs (R0–R7): all passing.** The bundles load; the process can reach the server; the editor opens
and **the agent is told which rack is open** (item 7); the **chip takes effect without a reopen**; the
**composer empties on send**; **closing mid-reply keeps the user's message and lands the reply** (item 5);
**reopening returns to the Link you were on** with **no stale release banner** (items 5 and 6); the rack OUT
reads back; **the pre-gain double-click restores AUTO** (ruling 1); the slot double-click and the note band
are wired (ruling 3).

**One leg SKIPS, loudly:** closing mid-reply on a **plain chat** turn. B's dev account is Free tier with a
daily pool of five chats, and the rig's own passes spend them; `sendChatMessage` then refuses before any
network and pushes the limit notice as an assistant row. The agent path is unmetered, which is why every
capability row runs. The leg now reports `chatsUsed=N of 5` and skips rather than blaming the plugin — and it
PASSED earlier in the night against the mock, where the pool is not a factor.

## What the loops found in the product (all fixed)

1. **The network shutdown latch.** `beginShutdown()` set a process-wide stopping flag when the last
   `EchoJayProcessor` was destroyed and **nothing ever cleared it**, so once the last EchoJay in a host
   process had gone, **every later request in that process was refused before it was made** — chat, the
   config fetch, the agent, silently dead until the host restarted. In a host the shape is an AU validation
   pass, or removing the last EchoJay from a project and adding one back. Cleared on the 0 → 1 edge now.
2. **A reply landing after a reopen was dropped by both sides.** The keep-rule asked "is an editor
   attached?" — it was, but a *different* window that had never sent the turn. It now asks whether the editor
   that SENT the turn is the one attached.
3. **Item 5's view restore never ran on a cold first selection** — it was recorded inside
   `handleBorrowSelectionChange`, which returns early for a ViewOnly decision. Recorded at
   `selectRackForView` now, the one door that moves the view.
4. **A reopen could create a new chat over a live one and wipe the transcript** — the adoption of
   `processorRef.activeChatId` only ran when the workspace store already knew that chat, so a chat started
   this session was replaced by a new empty one. Adopted at both sites now.
5. **A reload discarded rows the store had not caught up with** — the user's row is appended locally and
   synced, and a sync does not finish in 400 ms. The reload now keeps what the processor holds.
6. **A message held for the plugin scan was lost if the window closed during the hold** — the hold re-enters
   through a SafePointer, so a dead window meant the words vanished with nothing said. The transcript records
   them before the hold returns.

## Four faults in the rig itself, found by running it

Recorded because a rig that lies is worse than no rig: it wrote the wrong settings keys (`apiEndpoint`
instead of `endpoint`) so the plugin was never logged in; it wrote no usage fields so the first send was
refused locally; its waits re-read a **stale state file** instead of asking for a fresh one; its "atomic"
publish used `deleteFile + moveFileTo`, which worked exactly once and then froze the file at the first
snapshot (`forSeq=1` in the row detail is what finally named it); and one row read an `st` it never declared,
binding to a snapshot from the start of the pass. Each is fixed, and the state now carries the command
sequence that produced it so a stale read is impossible.

## Next

C10 is B's and is in `docs/FOR_B_2026-10-14.md`. The plain-chat close leg needs an account with a chat
allowance — also B's. Everything else on `docs/AGENT_CAPABILITIES.md` that the rig does not yet drive (the
goal rows G1–G6, the approval rows A1–A5, the reading rows R1/R2/R5–R7, F1/F3/F4/F6) is the next widening.
