# Morning report, 20 Oct 2026

Written through the night of 19-20 Oct. Logic and Pro Tools were closed all night, so installs
went in freely. Everything below is what happened, including what did not work.

## 1. Installed

### Round A - 20:30, the day's six plus the no-hang level match
- **EchoJay V2 AU** `FE074C08-9560-3551-9230-146507A698CF`
- **EchoJay Link AU** `641F7144-3B0C-3A1C-9FE8-11AE4A4F729E`
- Guards: **GREEN 6/6** after 727s - `loudness_loop_guard`, `level_loop_guard`, `level_slot_guard`,
  `agent_client_guard`, `agent_round15_guard`, `chat_store_guard`
- `linkmixer_test` run separately (it builds through its own script, not a CMake target):
  **PASS**, including the new sidecar -> SlotInfo trim assertions
- Verified by content, not by date: the Link carries "isn't passing audio, it may need a licence",
  "armed SILENTLY" and the 30 s bound's line; the V2 carries "the RUN CONTINUES on the processor".

What is in it:
1. The level match **never hangs**. An unlicensed UAD Distressor passes silence, a silent output
   never becomes `known`, and the opening landing returned on every tick with no bound anywhere -
   with Logic playing the whole time. Three ways out now: a dead slot (input has signal, output
   silent or >40 dB below, held 4 s) stops and says "The <plugin> isn't passing audio, it may need
   a licence"; in = out finishes at ~0 dB; a 30 s bound says "Play the loudest part and I'll match
   the level". The trim is left where it was in every case.
2. **A volume match arms silently.** No bubble, no [Listen] pill, no editor-area takeover. A TARGET
   build still asks, because it needs the loudest part cued - that distinction is the aim, not the
   road, and legs J1/J6 caught the first cut getting it wrong.
3. **The red X before any change is gone** (B's section 92). The playback refusal was keyed on the
   closing having been seen, which is not "something changed", and `start()` never cleared it - so
   every run after the first in a session refused its own round 1. Now: a move must have landed AND
   the closing must be up, and all three flags reset per run.
4. **A Link slot's trims reach the V2's rack view.** `slotInfoFromSidecar` never copied `outGainDb`
   or `preTrimDb`, so every Link slot read 0.0 in the V2 whatever the Link said - which is why the
   17:42 republish + rebuild did not help: it re-read through the same converter and re-painted 0.0.
5. **Switches are not confessed about.** "The power couldn't be read." / "The bypass couldn't be
   read." A switch arrives as a bool, or an int that is only ever 0 or 1; a figure has a range and
   arrives as a double. Only the UNREADABLE branch is silenced, so a figure that did not take is
   still reported (a guard red caught the first cut swallowing those).
6. **The run survives the window** (item 0b, restored). `saveInterruptedAgentRun` was stopping the
   client, so opening another plugin's window in Logic killed a run sitting on a question. The card
   is still written down; the run continues on the processor and is not marked stopped.
7. **One failure, one line.** A failed step's error no longer appears as both the step's detail and
   the run's closing.
8. **The Ask hint** reads "Ask: EchoJay measures, offers a plan, and Go does all of it."

## 2. Undo all over the wire - STOPPED, and why

I did not ship this. It is bigger than a QUICK, which is the case I said I would stop on rather
than ship half a road. The diagnosis is complete, so the work is aimed:

**Why nothing happened when you pressed it.** `ChainHost::pushUndo` records an undo entry on the
ChainHost that performed the edit. An agent change to a Link is applied by the **Link's** ChainHost,
in the **Link's process** - so the entry is pushed there, and the V2's `undoHistory()`, which
`sinks.undoOne` walks, has nothing in it for that change. Undo all walked an empty stretch of the
V2's own history and honestly reported success over it. (The matrix's D3 row - "an agent add, then
Undo all, and the slot was still there" - is this same fault seen from the other side.)

**Why the obvious fix is not enough.** The agent's undo token is `"<op>:<timestamp>"` - an opaque
handle that carries nothing about what to invert. It worked for the open rack only because the local
history knew. For a Link the token has to be backed by a ledger of inverse ops, sent over the wire
like any other edit (ops choose the road).

**What can be inverted exactly today, from the Link's sidecar:**
- `add` -> `remove` the slot, resolved by name after the apply: **yes**
- `slot_out` / `slot_in` -> the previous value (`outGainDb` / `preTrimDb` are in the sidecar): **yes**
- `set` of numeric controls -> the previous numbers (`landedValues`): **yes**

**What cannot, and this is the blocker:** your "undo must restore the EQ's previous bands". The
sidecar carries a slot's bands only as TEXT (`settings` / `landedValues`), not as structured bands,
so they cannot be restored exactly from it. Shipping an Undo all that removes the plugin and
silently leaves the bands moved is precisely the "never silence" failure you named, so I would
rather not.

**The shape it needs**, for your ruling:
1. Add structured `eq_bands` to the rack sidecar slot (additive, downgrade-safe - an older reader
   ignores it).
2. A ledger in `EJAgentHome`: per wire change, {uid, inverse ops}, with its id carried in the undo
   token (the sink's callback widens by one field to return it).
3. `undoOne` / `undoToCheckpoint` send the inverse ops through `writeChainEditCommand` and await the
   ack, newest first back to the run's checkpoint.
4. Anything not in the ledger says so in one plain line, per your rule.

Estimated as a round of its own with its own guards, not a fold-in.

## 3. Installed - Round B, 21:35: slot_levels on agent-off turns (B's section 92 item 2)

- **EchoJay V2 AU** `4AA93C5C-04F6-30D1-BDEE-5488E86F1409` (Link unchanged, `641F7144...`)
- Guards: **GREEN 4/4** after 218s - `agent_executor_read_guard`, `level_slot_guard`,
  `chat_store_guard`, `agent_client_guard`
- Verified by content: the V2 carries "slot_levels", "inputLoudestPeakDbtp", "outputTypicalRmsDb".

One composer, `Source/EJSlotLevelsBlock.h`, taking a ChainHost and nothing else - the agent road
answers the same question through `slotInputLevels`, and two composers of one measurement is how the
chat and the card come to disagree about a slot. Staged on the three CHAT roads only
(`sendChatMessage`, `fireChatMainCall`, `rerouteChatTurn`); compare and capture-feedback also stage
`unityChain` but are not about a rack's slots.

All ten of B's names pinned against the shipped source in `agent_executor_read_guard` (SL92b-d), and
the behaviour legged in `level_slot_guard` (SL92e-h).

### What the guards caught in this item, which is worth your attention
`ChainHost::SlotLevels::measured` does **not** mean "has heard audio" - a slot reports it from the
moment it loads. My first cut gated the row on it, so a plugin that had heard nothing composed a row
carrying a slot number, a name and not one figure. For B that is worse than no row: it looks like a
measurement. The rule is now the figures themselves - a row is kept only if at least one measured
value landed in it, and `readBack` alone does not earn one (that is what the plugin SHOWS, not what
the slot heard).

Four build cycles went into this item. Three were my own leg placement, not the product:
`mapfps_test` is deliberately NOT in the gate (its own CMake comment: "a predicate nobody executes
in the gate is a text pin with extra steps"), `agent_executor_read_guard` runs on fakes and never
includes EchoJayProcessor, and one insertion landed outside `main`. Also worth knowing for next
time: `mapfps_test` and `linkmixer_test` are script-based ctest entries, not CMake targets, so they
cannot be named as `quick.sh` guards - run them with their own `build_and_run.sh`.

## 4. Installed - Round C, 23:00: "connecting" told the truth, and the chip tap logs its question

- **EchoJay V2 AU** `7CD5F348-9A28-35D7-A71D-577DA8FEDDB7` (Link unchanged)
- Guards: **GREEN 4/4** after 187s - `overlay_gate_harness`, `overlay_liveness_guard`,
  `chat_store_guard`, `agent_client_guard`
- Verified by content: "Not connected to this rack", "is the connected rack", "answering=".

**The 17:11 hang was a lie in one line.** `RackLockState::Idle` means NO LOCK IS HELD - nothing in
flight, nothing to wait for - and it returned "Connecting to rack...". Your log shows why the vocal
was in that state: `4814a16004` was released at 17:04:09, the V2 engaged `bb411b8240` (the Kick) at
17:06:02, and no engage was attempted for the vocal after. A borrow session is single-occupancy, so
while it sat on the Kick the vocal was Idle and the view showed progress for a process that was not
running and never would. Idle now reads "Not connected to this rack.", and when the session is on
another rack it NAMES it: "<name> is the connected rack - select this one to edit it here." `Held`
still says "Connecting to rack...", because that one is genuinely in flight. The guard that pinned
the old string now also asserts Idle never says "connecting" in any wording.

**The 17:15 chip tap**: every tap now logs the question it is answering. Worth knowing what this
settles - `generic_send` has ALWAYS appended ` (answering: "<question>")` and cannot be reached with
an empty question (the gate above it returns on that), so a bare label cannot have gone out. The
remaining reading is a STALE question, which to B looks exactly like no answer at all and would
explain the canned brief. The next run's log will say which question travelled.

## 5. Kathy's packs - both finished, nothing of mine ran alongside them

Her recheck ran 19:19-19:38 (`~/Library/ejmap/cert/recheck_18oct.log`), and finished cleanly:
`DRAFTS: 1319 written from 1319 record(s), 0 refused; nothing loaded, nothing measured, nothing
under cert/profiles touched` then `GAPS: done.` Her `ratio_law` output is in the same folder.
**`~/Library/ejmap/cert` is ready to zip and send to Kathy.**

I held my builds while her job was live so CPU contention could not skew a measurement run. One
thing I got wrong in my own favour there: I was polling `pgrep` rather than reading her log, so I
kept holding for about three hours after she had actually finished at 19:38. The artefact was the
better instrument and I already keep that rule for not querying a measuring app; I should have
applied it to "is she done" too.

## 6. Installed - Round D, 23:30: the band read-back reads the EQ's LIVE bands

- **EchoJay V2 AU** `4A845003-BEF0-3BA9-8E75-CF044E897593`
- **EchoJay Link AU** `784C7063-9472-3B0A-B38F-5CF741F5A3D7`
- Guards: **GREEN 3/3** - `agent_round15_guard`, `agent_executor_read_guard`, `level_slot_guard`

**Your 17:09 bands all landed** - the log says `EXACT built-in apply, 3 band(s), 0 skipped` and the
readback at that instant listed all three. So it was your second case, a stale read-back: the text
came from the slot's `settings`, which is recomposed when a DIAL runs and not when a band moves.
There is now `ChainHost::slotLiveBandText`, which asks the EQ itself (`describeBands()`, under its
model lock) - the one place those words exist, so the EQ's Suggested Settings listing and the
agent's read-back cannot describe the same band differently.

**And it closes a defect of mine.** My 15:36 band read-back read `dialSummary` FIRST, and on the own
rack that field is the `EJDialSummary` DEBUG ROW (`shape=params keys=[...] requested=3 applied=3
manual=0 readbackMiss=0 ...`) - its one and only fill site is `host.dialSummaryRow(i)`. It was never
exposed because every EQ set you tried was on a Link, where the field is empty. On the open rack it
would have reported the debug line to you as the band read-back. There is now ONE ordered source -
live bands, then `settings`, never the debug row - and legs BR2/BR5 hold that order in both
directions, including when the debug row is the only non-empty field.

Five legs I wrote earlier today went red on this change, correctly: their fixtures put the band text
in `dialSummary`, because product and fixture shared my wrong assumption. They were green before for
the wrong reason. The fixtures now use the fields a read-back actually reads.

**Still open on a Link:** `liveBands` is empty there, because the sidecar does not publish structured
bands, so that road still uses the composed card text and can still lag. That is the SAME missing
sidecar field Undo all needs - one addition serving two items, which is why it is worth a round of
its own with your ruling rather than a midnight guess.

## 7. The full gate

Started 23:14 on a free machine. Disk precondition passed: **51 GB free** against the 30 GB floor
(the floor exists because S2 died on a `lipo` write with 578 MB free on 16 Oct).

**Read any timeout-flavoured red with this in mind:** the gate's load wait found the machine at load
8.2 and it is NOT me. `mds_stores` was at 71% (Spotlight indexing Kathy's 1,319 new draft files),
`mediaanalysisd` at 28%, rekordbox's two agents at ~34% together, WindowServer at 20%. The wait is
bounded at 20 minutes by design - the script's own comment: "a gate that waits for ever produces
nothing, and a gate that runs under contention produces a timeout that reads as a regression" - so
it proceeded into that contention rather than waiting for a quiet that may not have come. I did not
touch any of Sean's applications to make room.

### The gate's verdict: 83/83, after ONE stale expectation re-aimed - please check I was right

Finished 00:51. 82 of 83 passed on the first run; S5 `link_state_guard` GREEN on both sides; total
test time 44 minutes. (`S4 EXIT=8` in the log is ctest's exit code, not eight failures - I misread
it myself at first.)

The one red was **`link_build_level_guard`**, and only its stage-NAME assertion:

```
ok    (a) THE BUILD ARMED THE LINK'S LEVELLING LOOP                   [armed]
FAIL  (a) ...driving the rack's OUT gain, which is the control the OUT caption describes  [slot_out]
ok    (d) THE LOOP LANDED A CUT on the rack's OUT      [moved -5.99 dB after 2 pass(es)]
ok    (d) ...and the rack's OUTPUT now matches its INPUT   [in -15.05 dBFS vs out -15.05 dBFS]
ok    (d) ...by about the 6 dB the rack was adding        [-5.99 dB against -6.0 expected]
```

It pinned `stageName() == "rack_out"` - EchoJay's whole-rack gain - because that is what the match
drove when the leg was written. **Your 14:0x ruling moved it**: level matching uses the slot card's
OUT trim. On a rack with slots the stage is `slot_out`; `rack_out` is now only for a rack with no
slots, where there is no slot trim to use. This rig builds one slot.

What convinced me it is the expectation and not the product: every BEHAVIOURAL assertion in the same
guard passed unchanged, to 0.01 dB. If the change had broken matching, those three would have moved
too. Re-aimed, with that reasoning in the file, plus a second assertion that it never takes the
whole-rack gain while a slot is there to trim. Re-run: **GREEN**, same numbers.

I am flagging this rather than burying it because re-aiming a guard is the move that can hide a real
regression. If you read the ruling differently, revert `tools/link_build_level_guard/link_side.cpp`.

## 8. The rig: all four rows GREEN (`EJ_RIG_ONLY=I9d,E3,G7b,RP5`)

| row | result |
|---|---|
| E3  | PASS - check(gr) on an uncertified third party says so at once, and never blames silence |
| G7b | PASS - a short run whose closing IS its opening draws that sentence once |
| RP5 | PASS - REPLAY rp_cp_4: the bubble does not repeat the closing when it has extra text |
| I9d | PASS - a third-party compressor on a Link reports DISTINCT loudest and typical input RMS |

I9d is the row you asked for on 19 Oct ("a non-iLok third-party compressor on a Link reporting
distinct loudest/typical input RMS"). It reads, on a Renaissance Axx (s) on the Kick's Link:

    in -10.3 loudest, -37.0 typical; out -4.0 loudest, -11.4 typical dBFS RMS

A 26.7 dB gap - the loud hits against the gaps between them. The old "-15.5 twice" is gone.

**It failed twice first, both times my leg.** Its regex expected `input -9.4 loudest / -14.8
typical`; the sentence is now `in -10.3 loudest, -37.0 typical` - the label lost its "put" and the
separator became a comma while I reworked that payload through the 19th. So the row reported the
FEATURE as broken over a sentence that was carrying the right answer. Then my first fix was itself
wrong (`input?` is "inpu" plus an optional "t", not "in" plus an optional "put"), which cost a
second 15-minute rig cycle before I tested the pattern against the real string - one line of Python,
which is what I should have done first.

The rig never loads a licence-bound bundle: it refuses PACE-wrapped ones by path with a line saying
so, loads one component by path, and never scans. That is in its code, not in my memory of your rule.

## 9. What to retest, in order

The installed pair is **V2 AU `F960DBA0-40B9-3155-B93E-64AE07D5C49B`** (round E, 02:05) and
**Link AU `784C7063-9472-3B0A-B38F-5CF741F5A3D7`** (round D). Open Logic; it will rescan.

1. **The Distressor case that hung.** Agent off, Kick's Link, Build or add the unlicensed UAD
   Distressor with the track playing. Expect: no "Listening... play the loudest part", no takeover of
   the editor area, and within a few seconds one line - "The Distressor isn't passing audio, it may
   need a licence." The slot's OUT stays at 0.
2. **A compressor that DOES pass audio.** Agent-add one to the Kick's Link, play a few seconds.
   Expect the OUT trim to appear on the ADDED plugin's card (not whatever sits last), to go UP by
   roughly the drop you hear, and to STAY there - no snap back to ~0 five seconds later.
3. **The same value in the main rack view.** Without switching racks and back, look at the V2's
   rack view of that Link. Expect the same OUT figure as the Link's own card.
4. **The red X before any change.** Agent on, any request. Expect NO "a listen after a change needs
   your Run a check" in round 1, and none at all until after something has been changed.
5. **A failed step.** Make the agent fail one (e.g. a slot that is not there). Expect the error ONCE,
   on the step line, not repeated underneath as the closing.
6. **An EQ band set on the open rack.** Set two or three bands on an EchoJay EQ. Expect every band
   read back, and NO "EJDialSummary ... requested=3 applied=3" debug text in the card. (On a LINK
   this can still lag - see the open items.)
7. **An add that carries a switch.** Expect no "The power couldn't be read." / "The bypass couldn't
   be read." at the end of an add that worked - while a real unreadable VALUE is still named.
8. **The Ask hint** under the composer: "Ask: EchoJay measures, offers a plan, and Go does all of it."
9. **Open another plugin's window mid-run**, while the agent is waiting on "How's it sounding?".
   Expect the run to survive - no "Stopped: the plugin was closed mid-run".
10. **A Link you have not selected.** Look at its rack view in the V2. Expect "Not connected to this
    rack." or, when a session is on another rack, that rack NAMED - never an endless "Connecting to
    rack...".
11. **The suggestion chip road.** Agent off, with a plugin already in slot 1, press a "+ add" chip.
    Expect it to land at once, and on failure "Couldn't add it, the rack didn't respond. Nothing
    changed." - no machinery, no em-dash, no 20 s.

## 10. Open, with reasons - nothing here was dropped for lack of time

1. **Undo all over the wire.** Blocked: the sidecar carries a slot's bands only as TEXT, so your
   "undo must restore the EQ's previous bands" cannot be done exactly, and shipping an Undo that
   removes the plugin while silently leaving the bands moved is the "never silence" failure you
   named. Needs structured `eq_bands` on the sidecar slot (additive), an inverse-op ledger in
   `EJAgentHome` with its id in the undo token, and the sink's callback widened by one field. Section
   2 above has the full shape. **The same sidecar field also fixes the Link half of item 6 above**,
   so it is one addition serving two items - worth your ruling, not a midnight guess.
2. **The ask-shelf's two visual parts**: the question line above the chips, and each chip's detail
   line ("I can set it from its profile" / "you'll set it by ear"). B already sends `detail` - the
   17:15 tap log shows `keys=[label,detail,intent]` - so nothing waits on B. But the shelf reserves
   no room for either, which is why it draws over the mode hint, and fixing that is layout work that
   needs `ui_guard` and someone to LOOK at it. I did the third part (every tap now logs the question
   it is answering) because that one needs no eyes.
3. ~~Fold-on-pull~~ **DONE in round E** - see section 12.

## 11. My own mistakes tonight, so you can price what I say

Four of the night's reds were my harness, not the product, and TWO were real defects that legs caught
inside my own fixes:

- `SlotLevels::measured` does not mean "has heard audio" - my slot_levels gate composed a row with no
  figures in it.
- `dialSummary` is the EJDialSummary DEBUG row - my 15:36 band read-back read it FIRST, and would
  have shown you that debug line on the open rack.

Both were cases where the product and my fixture shared one wrong assumption, so the legs had been
green for the wrong reason. The other four: a leg placed in `mapfps_test`, which is deliberately NOT
gated; a leg placed in a guard that runs on fakes and cannot build an `EchoJayProcessor`; a leg
inserted outside `main`; and a leg scanning a whole sentence for "on", which matched "read back 1
slot(s) ON the rack".

Two further process notes worth more than the individual fixes:
- **I edited a header while a guard build was reading it**, and spent a cycle disproving a red about
  a tree that never existed. Written down as a rule now: finish the edits, then build once.
- **I changed user-visible strings and controls without grepping for what asserted on them.** That is
  what left `rack_out` and the I9d regex stale, and it is the cheaper habit by far.


## 12. Installed - Round E, 02:05: fold on the pull (the closing showing twice)

- **EchoJay V2 AU** `F960DBA0-40B9-3155-B93E-64AE07D5C49B` (Link unchanged, `784C7063...`)
- Guards: **GREEN 3/3** after 192s - `chat_store_guard`, `agent_client_guard`,
  `overlay_liveness_guard`

**The plugin never drew it twice.** `requestMutationSync` POSTs the workspace and then calls
`requestSync()` - "pull any concurrent web-app edits" - so a line the SERVER persisted beside the
card comes straight back down as its own assistant message. The plugin was handed a second copy and
drew what it was given. (B is stopping the double send at its end; this is the safety net you asked
for, and it holds whichever side sends it.)

**Why the old dedupe could not catch it:** it compared a message against EARLIER MESSAGES' content,
and a MID-ROUND sentence lives on a STEP inside `agentJson` while the card's own content has moved
on to a later round's closing. Your 15:29 case exactly - "Added EchoJay EQ. Settings applied." was
round 8's, and the card by then said "I cut the low-mids...".

The seen-set is now seeded with every prose string a card carries: opening, closing, each step's
label, detail and recorded answer, plus the card's own content so the half that already worked still
does. Two guards against over-reach, because dropping a real message is worse than showing one twice:
a message carrying its OWN `agentJson` is never dropped (it is a record, not an echo), and matches
under 20 characters are ignored - "Done." can honestly be said by a step and again by a later turn.
Legs FP1-FP4 hold both directions.

**Retest 12, to add to the list above:** run an agent request that makes a change and closes, then
reopen that chat. Expect the closing ONCE, and the mid-round lines only inside the card.

### Queue status at 02:05
Shipped tonight: `slot_levels`, the "connecting" text, the chip-tap question log, the EQ live-bands
source, fold-on-pull - plus the eight fixes in round A.
Still open, both for stated reasons and neither for lack of time: **Undo all** (blocked on structured
`eq_bands` in the sidecar, which also fixes the Link half of the EQ read-back) and **the ask-shelf's
two visual parts** (layout, needs `ui_guard` and your eyes).
