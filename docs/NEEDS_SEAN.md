# NEEDS_SEAN — decisions taken without you, and the few things a machine cannot judge

**16 Oct 2026.** Your rule: *"Anything you can't judge goes in NEEDS_SEAN.md with a PNG and one line.
Decisions go at the top, but take the most reasonable reading and keep going."* So the decisions are first,
each with the reading I took. Nothing here is waiting on you; if you disagree with one, say so and I will
turn it round.

---

## Decisions I took

### D1. A typed reply when there ARE choices on screen is a NEW INSTRUCTION, not an answer

Your blocker said *"a typed reply must never be silently dropped"*; your #21 said *"a new instruction
closes the waiting run (marked stopped) and starts a new run."* Both are now true, told apart by whether
the ask has anything to tap:

- **No choices at all** (the 09:19 case) — typing IS the answer, whatever `allowFreeText` says.
- **Choices on screen and the text matches one** — it is answered as a tap.
- **Choices on screen and the text matches none** — it is a new instruction: the waiting run is stopped and
  the text starts its own run.

Nothing is dropped either way, which is the rule both of your instructions share.

### D2. The rig will not load MTransient, because it is licence-bound

MTransient's bundle carries `__Pace_Eden.bundle`. Your standing rule is that nothing of mine loads a
PACE/iLok plugin (after the 2 Oct security prompt), and the rig now refuses one by name even if it is asked
for. So the matrix's "map exists but is wrong" case (P3) **cannot run in the rig** and is on the Logic
smoke-test list for you instead. The rig's third-party plugins are **Renaissance Axx** and **Vertigo
VSM-3**, neither of which is wrapped - and Renaissance Axx is the one you watched land at -20.0, which is
why it is the read-back proof case.

### D3. Links run in their own processes for channel scenarios, in process for audio scenarios

A separate process is the only way to prove the V2 is not reading a Link's memory, and three of this week's
faults lived exactly there. But Logic renders out-of-process AUs from ONE audio graph, so its V2 block and
Link block belong to the same render pass, while two rig processes pump their own blocks on their own
clocks. So: channel, lock, window and rack-view scenarios run on the two-process setup; audio, meter and
level scenarios run in process, where the block interleave is exactly Logic's. Every row in the report says
which. Full reasoning in `docs/RIG_LINK_CAPABILITY.md`.

### D4. "WORKING" leaves the tile when there is a GR figure

Your #16: *"fit it or drop the word."* "GR 10.9 WORKING" is fifteen characters in a 118 px row and never
fitted. A GR figure only appears when the device is reducing something, so the word says nothing the figure
has not already said - and it is the word that goes. HOT stays: four characters, and it is a warning.

### D5. The GR result carries no instant or single-transient figure at all

Your #17. A figure that must not be quoted has no business being sent, so `grNowDb` and `grSpikeMaxDb` are
gone from the result. `grPeakDb` and `peakDb` now **alias the loudest hits** (the 95th percentile), because
a prompt written against the old contract looked for a "peak" and found the transient. There is also a
ready-made `sentence` the model can quote.

### D6. Suggested Settings shows the landed value, or says it is not confirmed

Your #20, where the card and the step contradicted each other. The card was composed from the value that
was ASKED for; it now reads the plugin's own landed display where there is one, and labels the other case
"(sent, not confirmed)". For the record, in your Pro-C 3 case **the card was right and the read-back was
wrong** - the log has Pro-C 3 reporting "-18.00 dB", "4.00:1", "9.911 ms" from its own display.

### D7. After Undo all, the closing is replaced rather than edited

Your #14. Nothing on my side can know what the rack is now, only that what the closing claims has been
undone - so the sentence becomes *"Everything this run changed has been undone, so what it said above no
longer describes the rack."* The server's original closing is kept in the record, not shown.

### D8. A read-only run's control says "N steps"

Your #18, taking "show 'N steps'" rather than "no count": a run with four steps still has four steps worth
collapsing, and the control has to say what pressing it will do.

### D9. One scenario cannot be red against an old bundle, and the report says so

E7 (the GR meter's two labels never overlapping) is an assertion on a pure layout function that is compiled
INTO the runner, so when the runner runs against ship_2026-10-16a it still tests today's geometry. It is
marked in the report rather than counted as a reproduction. Everything else starred runs red against 16a.

### D10. The per-step Undo was there all along, behind the "2 changes" toggle

Pass 11 skipped matrix row D4 with a sentence about the product: *"the row drew no per-step Undo
control at all - every step's canUndo is false on the snapshot."* That was wrong, and it was wrong in
the direction that matters: it blamed the build for something the ROW had not done. A finished run
draws itself collapsed, and the per-step buttons are only laid out when the step list is open
(`if (v.stepsOpen || v.failuresOnly)`). There was no button because nothing had opened the list -
which is the first thing a person does before pressing a step's Undo.

The row now presses the toggle first, the same control you press, and then the step's own Undo. The
"queued: the per-step Undo has to be drawn" item is withdrawn from `docs/NEXT_ROUND_QUEUE.md`; there
was nothing to draw.

### D11. A row that cares which slots exist starts from an empty rack

Three rows failed pass 11 and not one of them was a product fault. The matrix runs in one process on
one rack, so by the fortieth row the own rack held seven slots left by earlier rows: A3's add had two
slots of the same plugin name to choose between, E3 asked about "the third-party slot" and got an
EchoJay Compressor an earlier row had left in slot 1 (so it answered with a figure, correctly, and
the row failed), and the legacy EQ row matched a second, empty EchoJay EQ by name and read its empty
settings. A row whose subject depends on what ran before it tests the running order, not the product.

So the rig hook gained `clear_rack` - the own rack emptied through the product's own `removeSlot`,
the same door your delete uses - and the three rows ask for the rack they were written for. It is
scaffolding, gated like every other rig command, and it cannot exist in a shipped plugin.

### D12. A gain-reduction check now says its answer on the row, not only to the model

The same pass drew a finished GR step as `[done]` with an EMPTY detail: the whole answer travelled in
the result for the model to phrase, so the row itself told you nothing. The result already carries a
ready-made sentence written for a person ("The loudest hits are about 4.4 dB of gain reduction and the
typical amount is about 2.1 dB."), because handing over the words is the surest way to stop a figure
being misdescribed. That sentence is now also the step's landed line. The model's instructions stay in
`note`, off the row.

### D13. Auto mode decides on undoability alone, including on another track — **RULED, KEEP IT**

> **Sean, 16 Oct:** *"D13: keep it, Auto runs undoable changes on other tracks without waiting, line
> names the track."*

So the build stands as it is, and the matrix line that said otherwise was mine: `AGENT_SCENARIO_MATRIX`
A11 is corrected, and the row (A11/A11b) now asserts the ruling rather than flagging it. The original
reasoning is below for the record.



The matrix row I wrote for A11 says *"no approval asked for a small move; add on another track still
asks per the mode table."* The build does the first half and not the second, and the reason is your
own 14 Oct ruling: in Auto an ask-first step runs without waiting **unless it cannot be put back**,
and that question is answered by `undoabilityOf` - the same function that decides whether the op
hands back an undo token. An `add` is undoable wherever it lands, so Auto runs it on a Link too.

What the build does guarantee on another track is that the line **names** it ("add a compressor with
a threshold - on 88_7even Kick"), because a line you approve must say what it acts on.

I have taken the reading that undoability is the whole decider, because that is what you ruled, and
the row (A11/A11b) asserts exactly that. If you want Auto to stop for anything that touches a track
other than the one you are working on, say so and it becomes one more clause in `undoabilityOf` -
about a dozen lines, with its own row.

### D14. "The settings did NOT take" was being said over a dial that had not finished

The joint live pass found this, which is exactly what it was built for. "Add Renaissance Axx to the
Kick's rack and set its Threshold", five times against the real server and the real model: three came
back confirmed and **two** came back *"Added it, but the settings did NOT take: slot 1 Threshold is
0.00 (asked -18.00)"*. The write had not failed. It had not happened yet - on a Link the round trip
runs V2 -> server -> V2 -> Link -> dial -> sidecar, and the read-back's six looks at 800 ms is not
always enough of it.

This is your morning item 5 ("the third-party read-back on a Link is broken") with its cause named.

Two things were wrong and both are fixed:

1. **The read-back was counting its own looks instead of watching the thing it waits for.** The dial
   now says whether it has finished - `DialStatus::pending`, carried to a Link's reader as an additive
   `dialPending` key in the rack sidecar - and the read-back waits while that is true, up to about
   twenty seconds, instead of giving up after four.
2. **The absence of a reading is not evidence of a failed write.** If the ceiling is reached while the
   dial is still running, the settings are reported as NOT CONFIRMED - "Added it. Settings not
   confirmed: the plugin had not reported them yet" - and never as "did not take". The sentence that
   blames the build ("this build cannot read that plugin's values back") is kept for the case it is
   actually true of.

Downgrade-safe: the sidecar key is written only when true, so an old build reading a new Link's
sidecar sees nothing, and a new build reading an old Link's sees `false` - which is the settled case
and exactly how this build behaved before the field existed.

### D15. The per-step Undo was not lost: a finished run collapses, and the button is on the line

You asked me to make sure. It is there, and here is exactly where: the per-step Undo is drawn on any
step that finished and handed back an undo token, and the step LINES are laid out only while the step
list is open. A finished run draws itself collapsed behind its "N changes" toggle - your own rule
("one agent run = one message", collapse to the closing) - so after a run ends you press the count
first and the line's Undo is there.

What changed this morning, and it is worth naming because it is the nearest thing to a loss: the old
rule was *"a failed run never collapses"* (`stepsOpen = stepsOpen || ! finished || anyFailed`), so a
run with a failure stayed open and its lines' buttons stayed visible. Your #13 said "Hide steps does
nothing on a failed run", so the press is honoured now and the collapse keeps the failed lines on
screen instead of refusing. A failed step has no undo token - there is nothing to put back - so no
Undo button went with it.

The matrix row D4 presses the toggle and then the line's Undo, which is what a person does, and the
evidence prints the drawn zones either side: pass 12 drew `[12,12,11:Hide steps,8:Undo all]` - two
per-step Undo buttons, one per undoable step. The pass-11 skip that said "the row drew no per-step
Undo control at all" was my row failing to open the list, and it is withdrawn.

### D16. Every Link in the rig was called "Untitled", and that was hiding a targeting question

Your two readings of the live evidence were the same fault: *"A4-live says Untitled's rack"* and
*"'Make the kick punchier' put a compressor on the MIX BUS, not the Kick."* The rig's Links had no
track name at all, so the model was working in a project where no track has a name. In one row it
said so - *"I don't see a track named Kick - did you mean one of these linked tracks?"* with chips
reading Untitled, Untitled 2, Untitled 3, Untitled 4 - and in the other it quietly used the channel
the chat was working on, which was the mix bus.

The names are real now and they arrive the way Logic delivers them: `updateTrackProperties`, the
host's own door, which `LinkProcessor` already treats as authoritative. The out-of-process pair are
"88_7even Kick_2_01" and "88_7even Vox_Lead_01"; the in-process pair are "88_7even Kick_2_02" and
"88_7even Vox_Dbl_01"; the V2 is "Mix Bus". The rig also prefers the Kick when it picks a Link, so a
row that says "the kick" is about the kick.

And B1-live is no longer only about whether a question can be answered - it now holds the whole
claim: **a goal that names a track is answered on that track.** If the run adds to the mix bus while
the kick is untouched, the row fails and prints both slot counts. If it still lands on the mix bus
with the names in place, that is a real targeting fault with evidence, and it goes to B.

### D17. A step's Undo is offered only where it is true — **RULED, ACCEPTED**

> **Sean, 16 Oct:** *"D17: accept, Undo all on multi-step runs, per-step Undo only where it's true;
> queue a redo-aware history for after launch."*

Shipped as described, and the redo-aware history is queued for after launch (`NEXT_ROUND_QUEUE`
item 10) rather than attempted now. The reasoning is below for the record.



D4 pressed one step's Undo and watched the OTHER step's value change. The sink says why: a per-step
undo pops the plugin-wide history ONCE - the last change - whatever token it is handed. For the last
step that is exactly right. For any earlier one the button says "undone" beside a line whose value is
still on the rack, and reverts something else.

"The last undoable step" was my first answer and it was still not honest enough. The plugin-wide
history **coalesces** consecutive edits (an entry whose `before` equals the previous entry's `after`
is merged into it), so a run that sets the pre-gain and then the master wet has ONE entry covering
both: pressing the last step's Undo took the pre-gain back to 0.00 as well - correctly, because that
is what the entry held, while the row said "undone" against one line and nothing against the other.

So the button is offered where its promise is exactly true: **a run with one undoable step**, whose
entry can only be that step's. Two or more are undone as a whole, by **Undo all**, which is what it
is for and which D3 tests. The same rule stands at the door (`undoStep` refuses otherwise and logs
why), so a stale snapshot cannot get round it, and D4 now holds both directions - a two-change run
draws no per-step Undo but does draw Undo all; a one-change run's own Undo reverts exactly it.

If you would rather every step kept a button, that needs a redo-aware history - a bigger piece than
tonight - and it is yours to call.

## Decisions taken overnight, 16-17 Oct (you were asleep; nothing is waiting on you)

### D18. Item 9 (Link meters to the main plugin) is QUEUED FIRST, not started tonight

Your words: *"ITEM 9, LINK METERS to the main plugin: start it. ... If it can't be done safely
tonight, queue it as the first item of the next round; don't let it block the install."*

It cannot be done safely tonight, and the reason is its shape rather than its size. "This track's
meters can't be read from this window" is true: a Link publishes a meter FRAME for its own channel
and the main plugin reads a rack sidecar, and the two carry different things. Making GR and the
listen window readable across that boundary means a new shared-memory frame per slot (gain reduction
has to be per-slot, not per-rack), a publisher on the Link's audio thread, a reader that can tell
"not published yet" from "zero reduction", and the certification rule riding with it so an
uncertified slot still shows nothing. Every one of those is a thing I would be writing blind at
midnight against a plugin I cannot open in Logic until you are awake, and the failure mode is a
figure that is WRONG rather than absent - which is the one class of fault you have ruled hardest
against all day.

So it is the first item of `docs/NEXT_ROUND_QUEUE.md`, with that design written out, and it is not
blocking tonight's install. The agent's sentence about it is honest in the meantime.

### D19. The "bring that Link's window forward" button is in, and it can only promise what it does

Your step 6 asked for the sentence "plus a button that brings that Link's window forward if
possible". The sentence is in. The button is in too, and here is exactly what it can do, because "if
possible" turns out to be the whole of it: this process cannot focus another plugin's window in
Logic. What it CAN do is ask the Link, over the control channel it already has, to bring its own
editor to the front - and that works only if Logic already has that editor open. So the button asks,
and then says which of the two happened: the window came forward, or "Logic owns that window - open
EchoJay on <track> and the plugin is in its rack." No button that silently does nothing.

---

## What the new runner found on its FIRST run, before any scenario

**Two EchoJay instances starting at the same moment can end up on two different channels, and neither
can see the other.** This is a real defect in the shipping build, not a rig artefact, and it is exactly
what the out-of-process Links were built to expose.

`LinkShm::resolveDir()` decides where the shared directory is by writing a probe file and then deleting
it. The probe was called `.probe` - **the same name in every process**. Two instances starting together
raced: one process's delete landed between the other's open and write, the write "failed", and that
instance quietly took the TMPDIR fallback instead. From then on it had its own registry and published its
rack where nobody was looking.

The proof, from the rig's own run: the Kick Link registered (`uid=45d483f290`, audio flowing, rack
published) and the V2 never saw it, because its `rack-45d483f290.json` was in `$TMPDIR/echojay_link/`
while the V2's was in `<state root>/Library/Application Support/EchoJay/link/`.

**In Logic this is the "my Link doesn't show up" shape:** several EchoJay instances initialise at once
when a project loads, and whichever loses the race disappears from the rack list. I have not seen it in
your logs, but the window is milliseconds wide and nothing would have told us - which is the part I have
also fixed.

Fixed three ways: the probe is `.probe-<pid>-<n>` so no two processes share it; a failed probe on a
directory that exists is retried once, because a transient failure is not an unwritable directory; and
taking the fallback is now logged loudly with its reason, so a split channel reads as a line in the log
rather than as a Link that never appeared.

### D20. The 5-attempt live pass was stopped after its 13 live rows, so the GATE could start

**Decided at 02:37, because the install is the hard requirement and the clock is the binding
constraint.** The joint live pass runs the thirteen LIVE rows first - the ones that need the real
model and five attempts each - and then re-runs the 39 deterministic scenarios against the real
model at model speed. All thirteen live verdicts were in; the remaining 39 had already been GREEN in
PHASE B against the mock half an hour earlier, and finishing them would have cost about 45 minutes
before the gate - which is what the install is gated on - even started.

So the live rig was stopped, the chain moved to the gate, and the live verdicts are in
`night_live.log` and in the morning report. If you would rather the pass always run to the end even
at the cost of the install, say so and the chain's order changes; I read your "don't give up on the
install" as ranking the install above the second half of a pass whose first half had already
reported.

### D21. `E10-live` was demanding a figure the product is right to withhold

The row I wrote for your item 2 asserted that "how much gain reduction is it doing" comes back with
a dB figure, and it went **0 of 5**. The live log says what the agent actually said: *"I added the
EchoJay Compressor with threshold at -20 dB to the Mix Bus rack, but I still can't pick up the gain
reduction"* - which is the **uncertified sentence**, the behaviour you listed under PASSED tonight
and the thing this tree spent a day building: no instant figure is published, and a reduction that
is not certified is reported as not certified rather than guessed.

The row was therefore demanding that the product break a rule you had just confirmed. Your ruling
(c) is about the ASK - *"if the transport is already playing with signal, listen instead of
asking"* - and that half is unchanged and still fails the row. What the answer must be is an
ANSWER: a figure, **or** a plain sentence saying why there is not one. Never silence.

Two of its five attempts also died with *"stream ended without a terminal frame ... The agent's
stream ended early"* - B's server, the same family as §52 - and those were discarded and retried
rather than counted, which is the rule for a turn the server never served.

### D22. RULED, 17 Oct: a plugin that failed before is OFFERED WITH A WARNING, never tried silently

His words: *"plugin that failed to load before -> say so up front, offer [Try anyway] plus the
loadable alternatives of the same role as chips. Never try it silently, never refuse outright."*

So neither of the two things I had considered. Not a silent retry (which is what happened this
morning - kHs was asked for by name, the add path did not consult the no-load list, and it failed a
second time with the same OS error), and not a refusal either (which is what my first cut of item 3
did, and would have taken away a plugin that might work after an iLok sign-in). It is a question,
with his options on it.

**How it is built, and which half is whose.** The plugin never attempts a remembered-failure load
unless the call says to: the add comes back NOT APPLIED with a plain sentence ("kHs Transient Shaper
didn't load on this Mac last time"), the raw reason kept for Details, and - in the result the model
reads - `tryAnywayOffer: true` plus `alternatives: [...]`, the loadable plugins of the same role.
B turns that into the ask with **[Try anyway]** and those alternatives as chips (FOR_B §22). When
the answer comes back as the same add carrying `tryAnyway: true`, the plugin loads it exactly as
named, which is the "never refuse outright" half.

The older ruling it narrows is still in the file beneath this one, because it was right about the
thing it was protecting: a plugin that has never failed is used exactly as named, with no question.

### D22b. The ruling this replaced (15 Oct): "a plugin the user NAMES is still used as named"

On 15 October you ruled that the remembered no-load list must never stop a plugin the user asked for
by name: *"a plugin the user NAMES is still used as named"*. It is written into the inventory source
in those words, and it is why **kHs Transient Shaper was tried again this morning** - you asked for
it by name, so the add path did not consult the list, and it failed to instantiate for the second
time with the same OS error.

Your item 3 today reads the other way: *"kHs (failed before) was tried again. Mark not loadable up
front from the live inventory."* Those two cannot both hold, so I have taken today's as the newer
ruling and narrowed yesterday's rather than deleting it:

- a plugin that has **failed to load in this session** is refused up front, by name or not, with a
  plain sentence saying so and (B's half) working alternatives of the same role as chips;
- the mark still clears the moment it loads for real, so signing into iLok gets it back;
- and a plugin that has never failed is still used exactly as named, which is what yesterday's rule
  was protecting.

If you meant something narrower - for instance "refuse it up front unless I name it twice" - say so
and it changes in one place.

### D23. What "replay the exact frames" could and could not mean this morning

Your method for this round was to replay this morning's real sessions rather than write rows from
shapes I imagine, and it worked: the tap-then-Apply defect is one line, and it was invisible to the
rig because the rig's scripted add carries no `plan` frame while the real one does.

Two parts of "exact" were not in my logs, and I want you to know which:

- **The start bodies.** The log records `POST /api/agent/start bytes=5699` and not the bytes. The
  fixtures therefore carry the goal, the target, the checkpoint and the chat the run belonged to -
  everything the client assembled the body FROM - but not the body itself.
- **The prose.** `message` frames were logged as "unknown event skipped" with no text, and two of
  your items are about those exact sentences. The two you QUOTED are in the fixtures with a
  `textSource` field saying they are your words rather than a recording; every other message frame
  is empty, which is honest.

Both are fixed at the source: the client now logs an unknown frame's text (160 characters) and the
protocol carries it, so the next replay is exact in both respects. Everything else in the fixtures -
frame order, tool-call ids, approval modes, plan frames, ask cards with their real choices, done
summaries, and your presses and typed answers with their timings - is lifted from the log.

---

## Needs your eyes (nothing is blocked on these)

These are the judgements a check cannot make. Each has its PNG under `docs/review/<date>/<scenario>/`.

1. **Does the run row read well now?** `docs/review/<date>/G6/editor.png` and `G7/editor.png` show a
   finished run with the build bubble gone and the closing only inside the message. The *words* are right;
   whether the row reads as one thing rather than a list is your call.
2. **Is the tile's caption legible at 6.5 pt?** `E2/editor.png`. "GR 4.4" fits where "GR 10.9 WORKING" did
   not, but I cannot judge whether you can read it at a glance on your screen.
3. **Does the ask hint read as help rather than as an error?** `B1/editor.png` - the amber line under a
   choice-less question: *"Type your answer in the box below and press Enter."*
4. **The mode hint's three seconds** - `G3/editor.png` catches it mid-life. Whether three seconds is long
   enough to read is a human judgement and always was.
