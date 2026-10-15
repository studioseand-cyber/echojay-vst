# NEXT ROUND — THE ORDER (Sean, 14 Oct, after his full Logic check of ship_2026-10-14a)

**This section is authoritative and replaces every earlier partial list in this file.** The detailed
findings below it are the investigations behind each item - every one of the seven fixes already has its
cause named from the plugin's own logs or its code, so next round is execution rather than diagnosis.

**Process:** investigate from the logs first (done for all seven), **every fix gets a rig row that FAILS
before the fix and PASSES after**, one full gate from S1, and nothing installed until it is green.

| # | fix | cause, already found | where |
|---|---|---|---|
| 1 | **HONESTY (top).** A `set` reported success having applied nothing, and the closing text claimed values | the payload arrived flat with no `"params"` wrapper, so the dial path applied **0 of 8**; and my read-back counted **slots**, which a `set` never changes | §5 |
| 2 | the 60 s client timeout kills a slow round and blames the user's internet | `EJAgentClient::connectTimeoutMs = 60'000` bounds the whole request. **Sean's number: 300 s or more, matching the server** | §0 |
| 3 | `check(gr)` fails with "no audio heard" while audio plays; 9 rounds | the per-slot picture it reads **has no writer anywhere**: read in 3 places, written in 0 | §1 |
| 4 | a hand pre-gain move is not undoable, and undo stops on a stale entry. Plus the release banner | no `preGain` undo kind; `UndoHistory::step()` consumes one dead entry and returns true. The banner came from **moving the view to the bus** | §2, §3 |
| 5 | after an agent build on a Link the main window reads 0 plugins until a reopen | the view takes `linkRackCache`, whose feeder is gated on `currentTab == Tab::Chain`; Sean was on the AI tab | §4 |
| 6 | a chat reply lost silently when the window closes mid-reply | `notifyChatTurnFinalised` is called **only with `true`** - a failed parked turn notifies nobody. **Folds into 0b** | §7 |
| 7 | the login screen's "Loading your dashboard" | `showLoginScreen()` never hides the dashboard surfaces, and `setSignedOut(true)` has one caller | §1 (first section) |

**Then, as queued:** item 0 + 0b + Auto mode to the approved mockup → multi-track jobs → 0a.

> ### WHERE THAT STANDS AFTER THE NIGHT OF 14/15 OCT
> - **the seven fixes: DONE**, gated and installed as `ship_2026-10-15a` (commit 947903ff).
> - **Auto mode: DONE** (commit 3a2e2fe1), with one deviation Sean may want to rule on - the mode line
>   floats just ABOVE the composer box, because there is no room under it (DESIGN_AGENT_IN_CHAT.md §6).
> - **item 0's surface (the panel becoming a transcript row) and 0b: NOT STARTED, and blocked** - their
>   rig legs cannot run until the rig can really close a window (see "OWED FROM SHIP 1" above).
> - **the plugin-choice card, the OUT knob, multi-track jobs, 0a: NOT STARTED.** The OUT knob's groundwork
>   is written up in §6 below: both racks clamp to -24..+12 dB, a Link's OUT is already one value.

**Rulings to build in:** plugin choice (§"CHOOSING A PLUGIN"), the OUT knob (§6 - **and the answer to
Sean's question is yes, on a Link it is already one value**), the unknown-track tap choices (below),
Ask-mode approval locking (B's, in FOR_B), and **undo in Auto without a second approval** (below).

## RULED: an unknown track offers the linked tracks as TAP CHOICES

> *"Unknown track: offer the linked tracks as tap choices, not 'type the exact name'."*

**There is an inconsistency between the two sides to fix on mine.** My unknown-channel refusal tells the
model (`EJAgentExecutorRead.cpp:151`):

> *"Name one of those, by uid or by its exact name, or stop - this list is complete and will not change by
> retrying."*

while B's own guard says *"Never ask the user for a uid or an id… If two tracks could genuinely be meant,
ask with their names as the choices."* **The two instructions pull in opposite directions**, and the model
echoes the shape of whatever it was last told - which is how Sean got "the exact vocal track name or uid" on
13 Oct. The list of names is already in that sentence (`knownChannelsSentence()`), so the fix is to tell the
model what to DO with it: *ask with these names as tap choices*. Never the uid, which the user never sees.

Row **U1**: a goal naming a track that does not exist produces an ask whose **choices are the real linked
track names**, with no uid and no "exact name" anywhere in what the user sees. (Today's L5 passes because
the model happened to do the right thing; U1 must assert the choices, so it cannot pass on luck.)

## RULED: in Auto, a user-typed "undo that" runs without a second approval

> *"Undo in Auto: 'undo that' typed by the user runs without a second approval. Redo can wait until after
> beta."*

This settles the open question in `docs/DESIGN_AGENT_IN_CHAT.md` §7 Q9 - and **not** by adding a redo. The
rule there was "a step that cannot be undone still asks in Auto", and an undo returns no undo token, so
Auto would have stopped to ask before an undo the user had just asked for out loud.

**The resolution: the user's own request IS the approval.** When the goal is an undo request - which B
already detects (`undoGoal`, and it refuses any other op on such a goal) - the undo op is pre-approved in
Auto. It still asks in **Ask** mode, like everything else.

`undoabilityOf` therefore grows one exception, stated where it is read: *an `undo` on an undo goal is
pre-approved in Auto because the user asked for it in words; everything else that cannot be undone still
asks.* **No redo op before beta.**

Row **A7**: in Auto, "undo that" runs with **no** approval wait and the rack returns. Row **A8**: in Ask,
the same request still waits for Apply.

---

## THE TWO PLANS THE NEXT SESSION SHOULD START FROM (written 15 Oct, 04:40)

### A. A HOST THAT CAN REALLY CLOSE A WINDOW (blocks item 0b, and converts three skips into coverage)

**Load the V2 as a VST3 for the lifecycle rows.** JUCE's VST3 host releases `IPlugView` by COM refcount,
which deletes the plugin-side editor deterministically; the AU hands back an autoreleased NSView whose
lifetime the host does not own, which is why one `EchoJayEditor` survives the whole AU run today.

Shape, in order:

1. `juce::VST3PluginFormat` + `findAllTypesForFile(results, "build-guards/.../EchoJay V2.vst3")` ->
   `createInstanceFromDescription`. The rig already registers the AU in-process; this is a second road
   beside it, not a replacement - the capability rows should keep running on the AU, because that is what
   Sean runs in Logic.
2. **One thing must change in the PLUGIN first, and it is small:** `publishRigProcessorState` writes a
   single `rig/rig-processor.json`, so TWO V2 instances in one process (an AU and a VST3) would both write
   it every second and the rig would read whichever wrote last. The file must be per-wrapper -
   `rig-processor-AU.json` / `rig-processor-VST3.json`, from
   `juce::AudioProcessor::getWrapperTypeDescription (wrapperType)` - and the rig reads the one belonging to
   the track it is asking about. **Today's single-V2 rig cannot hit this**, which is why it shipped as it
   is, but the second instance is exactly what this plan adds, so the rename comes first.
3. Then `Track::closeEditor()`'s bounded wait on `editorAlive:false` starts SUCCEEDING for that track, and
   the three skips (C11's and R4's "the plugin's editor was destroyed by that close") become real rows.
4. Only then are item 0b's legs writable: close mid-run / reopen mid-run / approvals and asks surviving a
   close / Stop after a reopen / no editor at all for the whole run / two windows.

### B. THE OUT KNOB (§6 below has the ruling; this is the order to build it in)

Groundwork already established, so nothing here needs rediscovering:

- **both racks clamp to the same range**: `LinkProcessor::kGainMinDb/kGainMaxDb` = -24..+12 dB, and the
  V2's `setBusGainDb` clamps to `kBusGainMinDb/kBusGainMaxDb`. One range for one knob.
- **a Link's OUT is already ONE value** (`gainDb_`, with `setGainDb` the single authority); the V2's own is
  `busGainDb_`. There is nothing to merge - §6's question is answered.
- **the write road to a Link already exists**: the monitor steppers send `{"gainDb": x}` down the ctrl
  command channel, and `LinkProcessor` applies it through `setGainDb` (LinkProcessor.cpp:1333).
- **the component to copy is `PreGainKnob`** (ChainWetKnob.h:200): bipolar, centre-detented, `userSet`
  amber, and `onResetAuto` is already the double-click hook. An `OutGainKnob` is its sibling with its own
  hue and caption, NOT a new control family.
- **the caption it replaces** is `EchoJayEditor::rackOutGainCaption()`, which already knows how to say
  nothing rather than claim unity - keep that rule for the knob's own label.
- **what the double-click needs that does not exist yet**: the loop must publish its last LANDED figure per
  rack, because "reset to the level-matched value" is otherwise a reset to 0.0 dB wearing a better name. If
  no match has ever landed, the double-click goes to 0.0 and says so.
- **the undo kind** is `rackOut` with a coalesce key, exactly as `linkGain` has, and on a Link the undo must
  re-send the ctrl command rather than write the V2's copy.

Rows O1-O6 are in §6. O2 ("the one value" claim) and O5 (undo reaching the Link) are the two that must be
asserted on the REGISTRY mirror rather than on the V2's own copy, or they prove nothing.

---

## OWED FROM SHIP 1 (15 Oct 2026): THE RIG CANNOT CLOSE A WINDOW YET

The C11 row took six runs, and three of them were my instrument rather than the plugin. The last one is the
one that matters for the queue:

**Releasing the AU's wrapper view does not destroy the plugin's editor in this host.** The rig's own
`unique_ptr` goes null, `v2.editor == nullptr` passes, and `EchoJayEditor` lives on - one built, one
destroyed, at the end of the process. So every row that "closes the window" has been testing the host's
wrapper, not the plugin, and two rows (R4 and C11's first cut) were passing because nothing had died.

Done in ship 1:

- `rig/rig-processor.json`, written by the **processor** on its 1 Hz timer, so a row can read `editorAlive`,
  `editorId`, `editorsBuilt`, `chatOwnerId`, `parkedChats`, `orphanedTurns`, `historyRows` and
  `lastAssistant` **with the window shut**. The editor-published `rig-state.json` freezes at the close.
- `Track::closeEditor()` waits, bounded, for the plugin to report `editorAlive:false`, and every row that
  closes a window now names the destroyed-editor half as a **skip with its reason** when it did not happen.
- C11 asserts the half that holds either way (a dead socket leaves an interrupted row, in the view AND in
  the processor's own record) and claims nothing about a destroyed window.

**Owed, and it blocks item 0b's legs:** a host that can really close a window. Cheapest first: load the
**VST3** for the lifecycle rows (JUCE's VST3 host releases `IPlugView` by refcount, which deletes the
plugin-side editor deterministically; the AU hands back an autoreleased NSView the host does not own).
Until then an 0b leg can only honestly be a skip.

### And one real defect it found on the way

The chat keep-rule compared **addresses**: `chatTurnOwner_ == getActiveEditor()`. A rebuilt editor landed on
the dead one's address (0x160008000, in the run that caught it) and inherited its claim, so a failed turn was
handed to a window that had never sent it. Identity is now a **counter** - every editor takes the next
`editorId` as it is built, and no id is ever reissued. That is the third costume this one bug has worn: "is
an editor attached" -> "is the SENDER still here" -> "is this the same OBJECT or just the same PLACE".

---

# Next round — the queue, in order

> ## 13 OCT 2026, SEAN'S RULING: THE CAPABILITY LOOP COMES FIRST, AND IT REPLACES ITEM 0 FOR NOW
>
> *"This sits after this round's 8 fixes and the dead path sweep, and replaces item 0 for now: the agent
> moving into the chat comes once it can actually do the work."*
>
> So the order is: the host rig (ask A, built 13/14 Oct — `tools/host_rig/`), then
> `docs/AGENT_CAPABILITIES.md` run end to end on it with B's local server, then
> `docs/AGENT_SCORECARD.md` after each pass, fixing mine and sending B its list, until every row passes or
> is blocked on a decision for Sean. Loops use the rig, not the full gate. The full gate (with S6) runs
> once the scorecard is green.
>
> **Items 0 and 0b below are therefore NOT next.** They are what happens after the agent can do the work —
> moving it into the chat is a change of surface, and a surface is not what is failing.


Rewritten 12 Oct 2026, after the round that took Sean's items 1-11.
Amended 13 Oct 2026, after Sean's in-host results on ship_2026-10-12b (eleven failures, all mine, all fixed
this round except the two already deferred — item 0 below and item 0a — plus ONE NEW deferral recorded as
item 0b: an agent RUN surviving the window, which is part of item 0's move and cannot be done before it).

## 0b. AN AGENT RUN MUST SURVIVE THE WINDOW CLOSING — part of item 0, 13 Oct 2026

Sean's item 4 on 12b was "a turn survives closing the window", for normal chat AND for an agent run.

**The chat half is DONE** (13 Oct): the editor hands its live `ChatStreamHandle` to the processor instead of
cancelling it, the socket runs to completion, and `onChatTurnFinalised` keeps the reply for the next window to
drain — including the case where the user reopens before the reply lands, which the first version of that hook
would have dropped a second time.

**The agent half is NOT, and it belongs here rather than in a half-fix.** An agent turn is not one request. It
is rounds of tool calls run against the *editor's* `ExecutorDo`, whose sinks reach this editor's panels and
whose `agentClient_` is an editor member destroyed with it. Parking the socket the way the chat stream is
parked would leave a session making decisions with nothing to act on — worse than stopping, because it would
look like it was still working.

Making it survive means moving `EJAgentClient` and `ExecutorDo` onto the processor, with the panel (or, after
item 0, the transcript) attaching as a listener. That is the same move item 0 makes. So: **item 0 does both**,
and until then the run stops cleanly when the window closes and says so in the log.

What already helps: the executor's sources are processor-bound already (`bindToProcessor`), and `ExecutorRead`
now carries an alive token, so a deferred completion cannot touch a dead executor.

## 0. THE AGENT IN THE CHAT, NOT IN A BOX ABOVE IT — Sean's ruling, 12 Oct 2026

Sean asked for this mid-round and asked to be told if it could not be gated safely alongside items 0-9.
**It could not, and he was told.** It is the first item of this round, whole, and it is not to be half-done —
his words, and the right call: a half-moved agent UI leaves the agent unusable rather than imperfect.

What it is, in his terms:
  * each agent turn is an assistant message in the transcript;
  * its tool steps ("read the rack ✓", "read the channel ✗ reason") are a compact collapsible step list at the
    top of that message — collapsed once the turn finishes, expanded while it runs;
  * questions and choices are buttons inside the message;
  * the plan card (Apply / Skip per line, Apply all) is inline in that message;
  * the status ("Thinking, round 3…") is a small line in the message while it runs;
  * Stop sits by the Send button while a turn is running;
  * error lines appear in the message in plain words;
  * everything stays in the scroll history and survives closing and reopening the window;
  * the separate agent panel above the chat is REMOVED.

**Why it is a round and not an afternoon.** None of the above is drawing work; all of it is the chat MESSAGE
MODEL. A turn needs nested tool steps with their own collapse state, inline buttons with hit-testing, an inline
plan card with per-line actions, a live status line, and every one of those serialised downgrade-safely so a
reopen restores them. That is the transcript architecture, plus deleting `EJAgentPanel` and rewiring its state
machine, plus a leg each (render-over-frames the way `chains_view_guard` does it, persistence, collapse/expand,
button hit-tests).

**What this round already did FOR it, deliberately.** Item 3 (the conversation surviving a reopen) is a
prerequisite, and it is done: `EchoJayAPI::onChatTurnFinalised` catches a turn that lands with no editor
attached, the processor keeps it, and the next editor renders and persists it. The restructure inherits that
rather than having to invent it.

**Start here:** `ChatMsg` in `PluginEditor.h`, and the four stores it documents. The field comments there
already state which fields persist and which must never — that discipline is what the new step/button/plan
fields have to join.

## 0a. THE REST OF ITEM 1: WHAT THE V2 STILL READS FROM THE MIRROR

Sean's item 1 was "every V2 display and measurement on an addressed Link must read the Link, not the mirror".
Part of it landed this round and part did not, and this is which:

**DONE.** The parameter VALUES shown (his test 9: chat set attack 8 ms / release 150 ms, the V2 went on showing
50 / 50). Cause: the V2's device editor reads the processor it is editing, which for a borrowed slot is the
mirror's instance, and the mirror was never told about a command the Link had confirmed. Fixed by
`borrowMirrorFollowAckedOps`, on the ACK and never on the send — a refused op must not move the display.

**NOT DONE, and these are the reasons.**

1. **The GR meters.** A dynamics meter is drawn by its own editor reading its own instance
   (`EedCompressorEditor.cpp:190` → `proc_.gainReductionDb()`), and for a borrowed slot that instance is the
   mirror's, which is in no audio path — so zero GR is the correct reading of the object being asked. It needs
   a NEW TRANSPORT, because the obvious one is closed: `LinkMeterFrame` is **128 bytes, fully spent, and its
   size IS the mapping stride** (`kRegSize`), so a field added to it moves every frame after slot 0 under any
   instance that has not been rebuilt. Even the last pad byte is taken (`short90Code`). The JSON rack sidecar
   is not the answer either: `publishRackSidecar` is gated on the rack PICTURE changing, so meter values would
   be either stale or forcing a republish at meter rate.
   So: a SEPARATE, versioned shm region for per-slot meters (GR, in, out), with its own stride and room to
   grow, opened ONCE and cached — the 11 Oct ring leak was exactly a per-tick descriptor and mapping. Then a
   `setRemoteGainReductionDb` override on the five dynamics processors (compressor, de-esser, expander, gate,
   limiter), cleared when absent, fed per slot off the existing ~30 Hz read. One leg, on RENDERED audio the way
   `link_live_edit_guard` does it: a Link compressing real audio, a V2 addressing it, the V2's reading inside a
   tolerance of the Link's own.

2. **The EQ analyser and the waveform.** A SECOND transport on top of the first: a per-slot spectrum, not a
   scalar. Note the EQ CURVE is already published (`RackSidecarSlot::curveDeciDb`), so the curve can be drawn
   from the Link today; it is the live analyser behind it and the waveform that cannot.

3. **The compressor "landing it as it plays" loop never finishing on a Link, and the calibration listening
   stuck on "Listening… play the loudest part".** Not diagnosed this round — flagged rather than guessed at.
   The place to start is whether the LINK is advancing the loop for a slot that is `controlled` (the sidecar
   flag meaning "leased to the main plugin"): the 11 Oct ruling made the Link the owner of the loop for its own
   rack (`calib_link_guard` asserts the V2 judges no window on a Link rack), so if the Link does not tick it
   for a controlled slot then NOBODY does, and both symptoms follow from that one gap. Confirm it in a log
   before writing any code — the last three rounds all had at least one defect that was a wrong guess about
   which side owned something.

4. **"What set it" for a Link's rack OUT gain (item 2's second half).** The figure is shown for both the V2
   rack and an addressed Link, and the provenance clause ("· level match") is shown whenever the V2's own loop
   wrote it. For a Link whose gain was set in an earlier session there is nothing that knows who set it:
   `EJLevelRecord` carries measurements only — no option and no landed gain. The caption shows the figure
   without a provenance clause rather than a guessed one. B is adding option/target/landed to the record; when
   it lands, `s.linkLevelling` and this caption are the two places to fill in.

## 0-superseded. The 11 Oct "V2's METERS AND GR" item is now **0a** above.

It is not dropped - it is restated with what this round learned, which the old text could not have said:
the transport it assumed (a field on `LinkMeterFrame`) is NOT available, because that struct is 128 bytes,
fully spent, and its size is the mapping stride. 0a carries the replacement design.

## 0c. buildNotes in the local result bubble — offered, not taken

B's done frame and one-shot body now carry `buildNotes` (Sean, 11 Oct: "if you'd rather draw those in the local
result bubble"). Nothing was done with it this round — it was an offer rather than an instruction, and the round
was already carrying five items. The result bubble is composed in one place, so this should be small.

## 1. THE BORROWED HOST AS AN OBJECT — the deferred half of the removals

Sean ruled on 11 October, on a recommendation: the mirror stays this round, and comes out next. The round
that just finished is what makes that safe, and the reason is worth reading before anyone picks it up.

The borrowed host is **no longer an audio path** — it neither plays nor measures, and nothing is fed to it.
What it still is, is the V2 editor's **read model** for a Link rack: slot names, dial maps, master wet, loop
state, the EQ editors, and which rack Save serialises. **119 direct uses** (74 in `PluginProcessor.cpp`, 45
in `PluginEditor.cpp`) and **15 test directories**, about ten of them gated.

Removing it means re-rooting every one of those reads onto the sidecar and every write onto a command. That
is mechanical *now*, in a way it was not a week ago, because nothing depends on the mirror being
**authoritative** any longer: every edit to a Link rack is a command sent when it is made, so the mirror and
the Link agree by construction rather than by a hand-back.

**The one genuinely new piece of work in it** is the plugin editor. `open_editor` exists as a command and the
Link honours it (stage 4), but the V2's card still opens the editor of *its own copy*, which is why a hosted
plugin's own settings are still the one thing that commits at deselect rather than as a command. Sending
`open_editor` instead is what finally makes "every edit is a command" true without qualification — and it is
also what lets the mirror stop holding instances at all.

Order within the item: the editor first (it is the only new behaviour), then the reads, then the deletion.

## 2. The ring's measured latency

`docs/LINK_REMOTE_CONTROL_PLAN.md` §10 lists this as not established, and it still is not: the ≤1 buffer
figure is from the structure of the code, not a stopwatch. The contract asks for the Link to log the
wall-clock delta between a frame's write stamp and the block that applied it, asserted at both buffer sizes.
The drain is wired; the measurement is not.

## 3. Tests that are neither gated nor excluded — **Sean's call, now one decision**

- `tools/mapfps_test` — registered nowhere and currently **19 failing assertions**, almost all text pins over
  source that has since moved.
- `tools/borrowhost_test` — 2,700 lines, **48 references** to the audio detour's machinery, which 11 October
  deleted. It was already unregistered before that round. Its exclusion is written in
  `tools/tests/CMakeLists.txt`; re-aiming it is a round of its own.

Gate them and fix them, or retire them. The suite's own rule is that a test is in the gate or carries a
written exclusion, and both of these were in neither state.

## 4. A monitoring question the removals raised — **Sean's call**

Soloing a Link from the V2 still **injects** that Link's ring into the V2's output (through-main on a
Mix/Master Bus main, nothing elsewhere). That was right when a selected Link's rack was switched off. It is
no longer obviously right: the Link processes in its own place and the solo fabric mutes the others, so the
soloed channel is already audible where it belongs — and the injection is a second copy of it, which is the
double-processing stage 1 forbade everywhere else.

It was **not** changed this round, deliberately: solo is a user-facing monitoring feature with its own spec
(`MUTE_SOLO_SPEC`) and its own legs, and quietly changing what a solo press does is the same class of act as
the original complaint. What *was* changed is its source — a soloed Link is now heard from its own ring
whether or not it is addressed, instead of sometimes from the V2's copy, so the figure no longer depends on
what the user has selected.

The question for Sean: does solo stay as an injection, or does it become the fabric's mute alone?

## 5. Flagged, needing someone else's decision

- **Sean:** the merged tree has **two** stores for reference sets — the kept preset feature and
  integration's new reference library.
- **Sean:** `level_slot_guard`'s summary separator moved with the integration merge (`gain_db 2.5`, not
  `gain_db=2.5`). Ruled 10 Oct: keep the re-aim, no product change. Noted here only so the change is not
  rediscovered as a regression.
- **B:** emit `option: "commercial"` rather than collapsing it to `pushed`;
  `docs/ECHOJAY_API_CONTRACT.md` does not state the current chat-stream refusal shape.
- **B:** `set_level` is now the **rack record**, not a Level slot. B's `CONTRACT_AGENT_TOOLS.md` should say so
  if it still describes a Level slot.

## Done this round, for the record

1. **The lease stops touching audio.** `rackLeaseEngage`/`Release` → `rackLeaseAddress()` with no
   counterpart; `rackLeasePrior_`, `leasePriorBypass_`, `rackLeaseMuteWant_`, `slotWetParkedSaid_`, the
   parked-rack arm and the prior remap gone; in `ChainHost` the whole bypass overlay (`setLeaseBypass`,
   `setAttachBypassed`, `attachBypassed_`), so `setSlotBypassed` is the one author and `intendedBypassed`
   moves with it. Six named bugs retire with it.
2. **The audio detour is deleted.** The ring consume, `borrowHost_->process`, the stamp-based alignment, both
   delay lines, the 16,384-sample budget, `borrowCtxMix_`, the injection, the in-context decision and its
   banners, the mute closed loop, the honorary strip, the monitoring fallback. **Addressing a Link now costs
   the host no PDC at all.**
3. **The measurer follows the audio.** The V2's copy was being fed a post-rack ring and processing it again,
   so a compressor calibration advanced on it was measuring doubly-processed audio — invisible, because
   stage 1 had already stopped anyone listening. The Link always measures and steps its own rack now.
4. **A build is a command.** The last V2 edit that was not. Nothing a user builds lives only on the V2 side,
   so `borrowedRacks` is no longer written — the reader stays for older projects.
5. **Three legs, Sean's.** `borrow_migration_guard` (never lost, never doubled, proven both directions),
   the interleaved-undo leg in `global_undo_guard`, and `downgrade_keys_guard` — the standing downgrade rule
   mechanised, so a dropped state key must carry a recorded reason instead of a sentence in a commit message.

---

# NEXT ROUND, as ruled 14 Oct 2026 (build order)

Sean's order, once `ship_2026-10-14a` is installed and he has tested it:

## 1. The login screen's "Loading your dashboard" (small, first)

**Diagnosed 14 Oct, not yet fixed** (the gate was testing the tree):

- `EchoJayEditor::showLoginScreen()` (`PluginEditor.cpp:3471`) hides ~30 components by name and **never
  hides `dashWebPanel_`, `dashView_` or `dashViewport_`**.
- `DashboardView::statusLine()` (`DashboardTab.cpp:571`) returns `{}` when `signedOut_` — but
  **`setSignedOut(true)` has exactly one caller**, `openDashboardTab()` (`PluginEditor.cpp:17719`). A logout
  or an expired token while the Dashboard tab is current therefore leaves `signedOut_ == false` and
  `loading_ == true`, and the view keeps painting *"Loading your dashboard…"* behind the login screen.
- `dashWebPanel_`'s visibility is gated on `signedIn` (`PluginEditor.cpp:24852`) but only re-evaluated on a
  re-layout, so it can outlive the switch to the login screen as well.

**Fix (both halves, because either alone leaves a way back in):** `showLoginScreen()` hides the three
dashboard surfaces, and signing out calls `dashView_.setSignedOut(true)` so the view's own text is empty
whatever is visible.

**Rig check:** start the rig with an auth file that is **not logged in**, open the editor, and assert the
published state reports the login screen with **no dashboard status line** — the hook needs a `screen` field
and the dash status line in `publishRigState` for that to be assertable on real state rather than on a
screenshot.

## 2. Item 0 + 0b, with Auto mode

`docs/MOCKUP_AGENT_IN_CHAT_APPROVED.md` (Sean clicked through it and approved it — **it wins where it and
the design doc differ**) with `docs/DESIGN_AGENT_IN_CHAT.md` for the mechanism. Includes:

- the agent run as one transcript message, steps ticking in place, per-line Apply plus Apply all,
  Details as a chevron disclosure, Stop on the running message, the finished message collapsing to its
  closing sentence with "N changes" and Undo all.
- **0b**: `EJAgentClient` + the executors move to the processor, the transcript attaches as a listener, and
  **the two build roads (`loadChainFromJson`, `sendChainToLink`) move to the processor** — the one sink of
  eight that an editor currently owns.
- **Auto mode**: the chip cycles Off → Ask → Auto, with the composer's one-line explainer per mode.
- the eight 0b rig legs plus the five Auto legs.

### Two things the mockup settles that the design doc did not

- **the glyph set grows**: the panel paints spinner/tick/cross/skipped/undone/stopped today; the mockup adds
  a **dim dot** (pending), an **amber ring** (awaiting approval) and an **amber question mark** (question),
  and asks for skipped/undone/stopped rows dimmed with undone **struck through**.
- **per-row right-hand status words** are specified: applied, auto-applied, skipped, not applied, undone,
  stopped, "your answer?", working. A question is a step row with answer chips under it, and tapping one
  records *"You said: …"* on the row.

### One thing to agree with B before building the explainer line

The composer's Ask line reads *"small moves just happen; adding, removing or touching another track waits
for your Apply."* That is a **statement of the approval policy, which is the server's to decide** — today B
marks each tool call `free` or `ask_first`. If B marks a small move `ask_first`, that line is wrong on
screen. Either B commits to that policy in writing, or the line is softened to describe what the plugin
knows ("some steps wait for your Apply"). **Asked in `docs/FOR_B_2026-10-14.md`.**

## 3. Multi-track jobs (MOVED INTO THIS ROUND, 14 Oct)

`docs/DESIGN_MULTI_TRACK_JOBS.md`. Sean moved it from "after" to **this round, before 0a** — B starts their
half immediately, mine follows item 0 because every visible part of it lives in the new agent message.
Rows M1-M7. The two open questions (the missing-Link wording, replace-vs-append) are being built to my
stated recommendations rather than waited on.

## 4. Item 0a, the Link deep-meter region

`docs/DESIGN_LINK_METERS.md`. One shm object per Link, GR + calibration + the analyser in **one push before
beta**.

## 0. FIRST, with the login text fix (found 14 Oct, 16:30, by the rig)

**The agent client's 60 s timeout kills a slow round and blames the user's internet.**
`EJAgentClient::connectTimeoutMs = 60'000` (EJAgentClient.h:160) is handed to `withConnectionTimeout`, and
on the agent stream it bounds the WHOLE request: a step that had already delivered its round-10 frames died
at exactly 60.000 s with *"Connection failed. Please check your internet connection."* The server's own log
has steps at 39.5 s, and a round with a thinking budget can exceed a minute.

- **300 s or more** (Sean, 14 Oct: "the server is now 300 s"), and say the bound in the log line so the
  next one of these is diagnosable in one read. The two sides must not disagree: a client that gives up
  before the server does turns a slow answer into a lost one, which is what happened here.
- and **separate the two failures**: "we could not reach the server" and "the round took longer than N
  seconds" are different facts, and only one of them is about the user's wifi. The second should offer
  Retry, which the client already supports (`retryable=1` was set).
- a rig leg: a scripted round held open past the bound must surface as a timeout with its own words, not as
  a connection failure.

---

# SEAN'S LOGIC CHECK ON ship_2026-10-14a (14 Oct, ~17:40) — THREE CAUSES, FROM THE LOGS

Investigated from `~/Library/EchoJay/logs/echojay-1.log` and `-2.log` only. Nothing built or installed.

## 1. `check(gr)` CAN NEVER SUCCEED — the slot picture has no writer at all

**Not an audio problem.** Two seconds before the first failure the plugin's own playback wait had succeeded:

```
17:41:27  EJAgent: playback wait ends (min reached) heard=8.0s played=1 line="Got it, you can stop."
17:41:29  EJAgent: step ... RUN check "check gr on slot 1"
17:41:29  EJAgent: step ... FAILED code=not_playing "No audio has been heard for gain reduction on slot 1 yet."
```

`check(gr)` gates on `s.pictureValid && s.grKnown`, which come from `ChainHost::slotPicture(i)` →
`slots_[i].picture`. **That member is read in three places and written in NONE:**

| read | at |
|---|---|
| `slotPicture()` (the agent's gr check) | `ChainHost.cpp:3129` |
| `slotPictureText()` (the caption under each slot in the strip) | `ChainHost.cpp:3130` |
| `listenCardLines()` (the Listen reply card) | `ChainHost.cpp:3155` |

Its declaration says *"filled at Listen"* (`ChainHost.h:2499`) and **nothing fills it**. So every slot's
picture is a default-constructed `SlotPicture` with `valid=false, grKnown=false`, for ever. Consequences:

- `check(gr)` fails on every slot, on every rack, however long audio plays. Sean's nine rounds were the
  agent retrying something structurally impossible.
- the per-slot caption in the chain strip has never shown a reading, and the Listen card says "no reading"
  for every slot.
- `check(level)` is **not** affected: it reads the channel meters (`out.known`), which is why levels work.

**Fix:** write the picture. The data exists - the dynamics devices already report GR (`EedDynamicsCore`
drives the compressor's own GR), and the per-slot in/out levels are measured for the Level slot and the
loop. One writer, on the audio-to-message hop that already publishes per-slot meters, stamped with the
structural revision the header describes (so a rebuild invalidates it rather than showing a stale reading).
**A guard must assert a non-empty picture after N blocks of audio**, and the rig must drive `check(gr)` to a
NUMBER, not to a refusal - today's H3/GR rows only ever assert the honest refusal on a Link, which is why
this survived.

**And the second half, Sean's product rule:** when nothing is playing, the agent should ASK the user to
press play, not fail. Today `notPlaying()` returns a failure and the model treats it as a dead end. It
should carry a `needs_playback` outcome whose alternative is "ask the user to start playback", and
`wait_for_playback` should be the model's instructed next move. (With the picture written, the common case
disappears - but the rule stands for a genuinely stopped transport.)

## 2. A HAND MOVE ON THE PRE-GAIN IS NOT RECORDED, AND UNDO STOPS DEAD ON A STALE ENTRY

```
17:46:31  EJPreGain: set 5.83 dB (by the user)      <- the drag. No undo entry is pushed.
17:46:33  EJUndo: Undo skipped: remove NLS Buss (s) - its target is no longer present
17:46:37  EJUndo: Undo skipped: remove UAD UA 1176LN Rev E - its target is no longer present
```

**(a) No, a hand move on the pre-gain is not an undo step.** `UndoEntry`'s kinds are chain / wet / trim /
keep / dial / gesture / linkActive / linkGain / alias / loop / group - there is no pre-gain kind, and
`setPreGainDb` pushes nothing. (Today's snapshot fix made a *chain edit* restore the pre-gain, which covers
the agent's `set_pre_gain`; a user's own drag still records nothing.) So Undo correctly reached past it - to
a dead entry.

**Fix:** a `preGain` entry kind, pushed by the knob's gesture with a coalesce key so one drag is one entry
(the pattern `linkGain` already uses, `PluginProcessor.cpp:6524`). Same for the master wet knob, which has
the same hole.

**(b) Undo consumes a dead entry and stops.** `UndoHistory::step()` pops one entry, applies it, and when
the apply fails it writes "Undo skipped: ..." and pushes the entry to redo - then **returns true**. One
press, one dead entry, nothing undone. Sean pressed twice and burned two.

**Fix:** on a skip, walk on to the next entry (bounded - say 10, and never silently past something that
WOULD have applied), and report what was actually undone: *"Undone: &lt;label&gt; (2 stale steps skipped)"*.
A skipped entry is not a user action; it should not cost the user a press.

## 3. THE RELEASE BANNER CAME FROM MOVING THE VIEW TO THE BUS

```
17:39:57  EJRackSel: row=bus resolved=(main) current=4814a16004 -> viewing the local rack
                     (R1: the view moves, the chat does not)
17:39:57  EJBorrow: session ACTIVE <- false (release, uid=4814a16004, keepEdits=N)
17:39:57  EJBorrow: session ACTIVE <- true  (engage begin, uid=4814a16004)
17:39:57  EJBorrow: cleared the sticky banner on engage (it said "Released - Aitch_4_01 owns its rack
                    again. Nothing was changed on either side.", and this engage supersedes it)
```

`4814a16004` is **Aitch_4_01**. Selecting the **bus** row moved the view to the local rack - which is the
25 Sep rule working correctly - and that released Aitch_4_01's borrow, **then re-engaged it in the same
second** because the chat's working target is still that Link. The release writes a *sticky* banner; the
re-engage clears it. Sean saw it in the gap.

**Fix:** do not write a user-facing release banner for a release that is part of a view change. The banner
exists to explain a deliberate hand-back; a lease churn caused by looking at another rack is not one. Either
suppress it when a re-engage for the same uid follows (it already detects exactly that - the "supersedes"
line), or better, do not write it on the view-driven path at all. This is item 6 of the 13 Oct round in a
second costume: the centre of the window reporting an internal event as if the user had done something.

---

# RULED (Sean, 14 Oct): CHOOSING A PLUGIN WHEN THE USER DIDN'T NAME ONE

> *"when the user doesn't name a plugin, in Ask mode offer up to 3 options from their own plugins that suit
> the source, each with a short note on whether EchoJay can measure it; in Auto pick the best fit, say why,
> and offer a swap; remember the pick per job type; a named plugin is always used as named."*

**A named plugin is always used as named** - this changes nothing about "add FabFilter Pro-C 2".

## My half

1. **The three options are a card in the agent message**, with the same shape as the multi-select ask from
   multi-track jobs (`docs/DESIGN_MULTI_TRACK_JOBS.md` §2): three rows, one tap each, no leaving the chat.
2. **"whether EchoJay can measure it" is a fact I own and must not guess.** Per plugin it is: is it a
   built-in (GR and levels readable); is it mapped (`dialCapable` on the Link, the maps registry server-side
   for a third party); or is it neither, in which case the note says *"I can set it up but can't measure
   what it's doing"*. That note must come from the same source the dial path uses, or it will drift from
   what actually happens on Apply - the `dialCapable` rule's own words: **the fallback is INCAPABLE, not
   unknown**.
3. **"remember the pick per job type"** is a new piece of persisted state: `{jobType -> plugin name}`, in
   the V2's own state beside `linkGroups`. Downgrade-safe by the sidecar rule (a new key at v:1 is never
   read by an old build). It must be a *preference*, not a lock: the card still shows the alternatives with
   the remembered one marked, because a remembered pick that cannot be changed is a trap.
4. **Auto says why in one clause** ("your 1176 - it's mapped, so I can dial it and read its GR") and offers
   a swap on the same row. The swap must be one tap and must not re-run the whole job.

## Rig rows

- **P1** no plugin named, Ask: exactly three options, each from the user's own inventory, each carrying a
  measurability note that matches what the dial path would actually do.
- **P2** the same in Auto: one pick, a stated reason, a swap offered; the swap changes the slot and nothing
  else.
- **P3** the remembered pick is offered first on the next job of that type, and is still swappable.
- **P4** a named plugin is used as named, with no options card at all.
- **P5** a user whose inventory holds nothing suitable gets the honest line, not a third-party name they do
  not own (this is the `echojay_only` / inventory path, already proven by R3).

---

# 4. THE MAIN WINDOW'S VIEW OF A LINK IS A 1 Hz CACHE THAT DOES NOT REFRESH ON THE AI TAB

```
17:48:41  EJAgent: build target uid=bb411b8240 slots=2  path=sendChainToLink
17:48:41  EJChain: sending to Link -- 509 ch, 2 slot(s), 2 with settings_structured  uid=bb411b8240
17:48:42  EJAgent: step ... DONE "the build was sent to that Link (2 slot(s)); read back 2 slot(s)"
17:49:11  EJRackSel: row=3 resolved=bb411b8240 -> viewing it
17:49:11  EJBorrow: read START uid=bb411b8240 slots=0 channel=per-seq      <- "Reading 0 plugin(s)..."
17:49:59  EJBorrow: read START uid=bb411b8240 slots=2 channel=per-seq      <- 78 s later, correct
```

**Two readers of the same file disagreed for 30 seconds.**

- the agent's read-back (`Sources::linkRack`) calls **`LinkShm::readRackSidecar` directly** - a fresh read.
  It saw 2 slots one second after the send, so the Link had built and written its sidecar promptly. **The
  build was fine and the read-back was honest.**
- the rack view's borrow read takes `st->slots = it->second.rack.slots` from **`linkRackCache`**, and that
  cache's feeder (`refreshLinkRackCache`) is gated at `PluginEditor.cpp:27509` on
  **`currentTab == Tab::Chain && chainListPanel.isVisible()`**, plus a pass when `editActive()`.

Sean was on the **AI tab** talking to the agent, so the Chain-tab feeder never ran: the cache still held the
pre-build 0 from whenever he last had the Chain tab open. Selecting the Kick then read that 0, said *"Reading
0 plugin(s) from 88_7even Kick_2_01…"*, and - because the lease was taken - the Link correctly said
*"Selected in the rack on EchoJay on Stereo Out"*. It came good at 17:49:59 when a refresh finally ran.

**Fix, two parts, both small:**

1. **The selection path must read the sidecar fresh**, not trust a 1 Hz cache: `readLinkRackSidecar(uid)`
   already writes through the cache (the "single source of truth" comment at `PluginEditor.cpp:33132`), so
   one call at the top of the borrow read makes the two readers agree *by construction* rather than by
   timing. The 1 Hz feeder stays for the strip.
2. **A build sent to a Link forces a refresh for that uid.** `sendChainToLink` is the one choke point both
   the agent and the Build button go through (this round made it so), so the forced refresh belongs there -
   not in the agent path, or the Build button keeps the bug.

**Rig row (Sean's ask):** **K1** - after an agent build on a Link, select that Link in the main rack and
assert the view shows the new slots. It must select **immediately** after the build returns, because a row
that waits a second would pass today; and it must assert the SLOT NAMES, not a count, so "2 slots of
something" cannot pass. A second row **K2** holds the cause directly: with the AI tab in front for the whole
build, the cache must still be right when the Link is selected.

---

# 5. TOP PRIORITY, HONESTY: A `set` REPORTED SUCCESS HAVING CHANGED NOTHING

Sean, from the Mix Bus: *"make the kick punchier"*. The plan card showed `set slot 2:  -` with an **empty**
settings description beside a fully described EQ line. He pressed Apply all. The EQ changed; the compressor
did not; the closing message said *"I opened the compressor's attack to 18 ms and tightened release to
85 ms… with a touch of makeup gain"*.

## What the set carried, and why nothing moved

```
17:52:14  EJDial: slot 2 ("EchoJay Compressor") BUILT-IN PAYLOAD NOT UNDERSTOOD -- device present,
          resolved neither accepted shape.
          got keys: [threshold_db, ratio, attack_ms, release_ms, knee_db, makeup_db, mix, mode]
          wanted: "params":{...} (or the device's own array form, e.g. "eq_bands")
17:52:14  EJParamApply: slot 1 ("EchoJay Compressor") EXACT built-in apply, 0 band(s), 0 skipped
17:52:14  EJParamApply: slot 1 readback -> threshold_db -18 dB, ratio 4, attack_ms 10 ms,
          release_ms 120 ms, knee_db 6 dB, makeup_db 0 dB, mix 100 %, mode clean
```

**The parameter NAMES were right** - they are the built-in compressor's own names, the same ones the readback
prints. **The SHAPE was wrong**: the values arrived as a flat object with no `"params"` wrapper, so the dial
path recognised neither accepted shape, applied **0 of them** (`0 band(s), 0 skipped`), and the readback shows
the untouched defaults. The empty settings description on the plan card is the same fault surfacing in the
UI: the card could not describe a payload it could not parse, and printed nothing rather than saying so.

## And my executor called it a success

```
17:52:14  EJAgent: step ... DONE undo=set:1791996734350 "the rack took it and now holds 2 slot(s)"
```

**"now holds 2 slot(s)"** - the read-back I built this round counts **slots**. For `add`, `build` and
`remove` that is the right question. For a `set` it is meaningless: a set never changes the slot count, so
the check is true before and after and cannot fail. I built a structural read-back and let the value ops
ride on it. That is the same fault as the nine sinks, one layer in.

## The fix, my half

1. **A `set` reads back the VALUES.** After applying, re-read the slot's parameters and compare each asked
   name against what it now holds (with a tolerance per unit). Report `not_applied` naming the parameters
   that did not move - *"attack_ms is still 10 ms (asked 18)"* - and `applied` only for what matches.
2. **"payload not understood" must FAIL the op**, not log a line and continue. Today it is a log line and
   the op proceeds to report success; it should be `bad_request` naming the shape it wanted, so the model
   can resend correctly in the same round.
3. **Accept the flat shape too**, since the names were right: when every key of the payload is a known
   parameter of that device, treat it as `params`. Liberal in what we accept, strict in what we confirm -
   but the read-back in (1) is what makes that safe, not the leniency.
4. **The plan card must never print an empty settings description.** If the payload cannot be described, the
   line says *"settings I can't read"* - which would have warned Sean before he pressed Apply all.

## Rig rows

- **S1** a `set` on a Link compressor's attack / release / makeup shows the **new values read back** from
  that Link's own sidecar, not a slot count.
- **S2** a `set` whose payload is in a shape the dial path cannot read **fails honestly** (`bad_request`,
  naming the wanted shape), and the rack is unchanged.
- **S3** a `set` whose payload is flat but whose names are all known **applies and reads back**.
- **S4** the plan card for an undescribable payload says so rather than showing an empty description.

---

# 6. RULED (Sean, 14 Oct): A REAL OUT KNOB AT THE END OF EVERY RACK

> *"replace the 'OUT -0.7 dB' caption with a real OUT knob at the end of every rack, main and Link.
> Double-click resets it to the level-matched value. A hand move marks it user-set so level match won't
> override it (like preGainUserSet). It is undoable."*

## The question answered: on a Link they are ALREADY one value

Sean asked whether the Link's LINK-tab GAIN fader and the rack OUT are the same value. **They are** -
`LinkProcessor::gainDb_`, with `setGainDb()` as the single authority ("clamps, mirrors to the slot,
dirty-marks, and syncs an open editor's slider"). **There is nothing to merge.** Four writers, one value,
one mirror:

| who | how |
|---|---|
| the LINK-tab fader (and its `linkGain` undo entry) | `writeLinkCtrlCommand(uid, "gainDb", …)` → `setGainDb` |
| a group level move | the same, per member |
| the agent's `set_rack_out` on a Link | the same (`PluginEditor.cpp:3136`) |
| **the level match itself** | the loop's actuator on a Link is literally named **`rack_out`**: `readDb` → `gainDb_`, `writeDb` → `setGainDb` (`LinkProcessor.cpp:82`) |
| the main window's view of that Link | reads the mirror, `RegistrySlot::gainDb` |

The Link has **no** separate bus gain - `setBusGainDb` exists only on the V2, for the V2's own rack. So each
rack has exactly one OUT, which is what the ruling wants; the knob is a view onto a value that is already
single.

**The consequence that matters:** because the level match's actuator IS this value, the user-set flag is not
a nicety. Without it the loop will move the user's own knob under their hand - which is exactly the
complaint the pre-gain's `preGainUserSet` was introduced to answer.

**One thing to confirm when building:** the V2's own actuator chooser (its `rack_out` branch) must be read
the same way - the Link's is confirmed above, the V2's needs the same check so "double-click resets to the
level-matched value" means the same thing on both.

## What to build

1. **The knob** at the end of the strip on both, replacing the caption. Same component family as the
   pre-gain knob, so the gesture rules come free (`ChainWetKnob`'s `doubleClickAction` already prefers
   `onResetAuto`, which is the hook for (2)).
2. **Double-click → the level-matched value**, which means *remembering* it: the loop must publish its last
   landed figure per rack, and the knob resets to that rather than to 0.0 dB. If the loop has never landed,
   the double-click goes to 0.0 dB and says so - never a silent no-op.
3. **A hand move sets user-set**, exactly as `preGainUserSet` does, and the loop must honour it: `armFromChain`
   should decline the `rack_out` actuator when the user has set it by hand, and **say why** in its aim line
   (the loop already explains itself there; an unexplained refusal to level is worse than levelling).
4. **Undoable**, with its own entry kind (`rackOut`) and a coalesce key so one drag is one entry - the same
   pattern as `linkGain` and the one this round's item 2 found missing for the pre-gain. On a Link the entry
   must re-send the ctrl command on undo, as `linkGain` already does.
5. **The three views move together**, which they will by construction once the knob writes through
   `setGainDb` / `setBusGainDb`: the Link's own rack knob, the LINK-tab fader, and the main window's view.
   Item 4's stale-cache fix is what makes the third one prompt rather than up-to-a-second late.

## Rig rows

- **O1** the knob on the **own** rack: set it, read it back from the processor, and the caption/value agree.
- **O2** the knob on a **Link**: set it from the Link's rack and assert the **registry mirror** and the
  **LINK-tab fader value** both move - the "one value" claim, asserted rather than assumed.
- **O3** a **hand move marks user-set**, and a subsequent level match leaves it alone **and says why**.
- **O4** **double-click** returns it to the level-matched value after a match has landed; with no match ever
  landed it goes to 0.0 dB and the line says that is what happened.
- **O5** **undo** returns it, one entry per drag (not one per mouse-move), and on a Link the undo reaches
  the Link - read back from the registry, not from the V2's own copy.
- **O6** the agent's `set_rack_out` and the knob write the **same** value: set by agent, read by knob, and
  the reverse.

---

# 7. A PLAIN CHAT TURN THAT FAILS AFTER THE WINDOW CLOSES IS SILENCE BY CONSTRUCTION

**Not in the logs.** Sean's turn is in no log file: the last chat send is 17:56 and the logs run to 18:02
with nothing after 18:00. So this was found in the code instead - which turned out to be enough, because the
cause is structural.

## First, a correction: the rig's close-mid-reply row is NOT agent-only

`R4` runs `workOn(linkUid)` and then `agentChip(false)` before it sends: it is a **plain chat turn on a
Link's chat**, closed mid-reply, and it passes against the real server. So Sean's configuration is covered
and the handover mechanism works. What R4 does not cover is the turn **failing** - and that is the whole bug.

## The cause: the finalise hook is a SUCCESS-ONLY notification

The 13 Oct fix was right as far as it went - the stream is **parked on the processor, not cancelled**
(`PluginEditor.cpp:3301`), and `onChatTurnFinalised` keeps the reply for the next window. But:

```
EchoJayAPI.cpp:1772      notifyChatTurnFinalised(reply, true);
EchoJayAPI.cpp:2330      notifyChatTurnFinalised(obj->getProperty("reply").toString(), true);
```

**Those are the only two call sites, and both pass `true`.** Every failure path - a stream ending without a
done frame, a non-2xx, the 60 s timeout, a dropped socket - returns to the **editor's** own callbacks, which
are `SafePointer`-guarded and no-op once the window is gone. So a parked turn that fails notifies **nobody**:

- no reply, because there is none;
- no error, because the only listener left was never told;
- no "interrupted" note, because nothing records one.

Silence, for ever, exactly as Sean describes. And the processor's own guard -

```cpp
if (! success || reply.trim().isEmpty()) return;    // nothing worth keeping
```

\- is **unreachable dead code for the failure case**: it can never be called with `success == false`. It
reads like a decision ("nothing worth keeping") and is in fact a branch nothing can enter.

## The fix

1. **`notifyChatTurnFinalised(text, false)` from EVERY terminal failure path** of both transports - the
   streaming one and `/api/chat` - carrying the reason. One notifier, all outcomes, which is what the
   comment above it already claims ("the one place a finished turn can be caught no matter what happened to
   the window").
2. **The `! success` branch records an INTERRUPTED row** for that chatId instead of returning - the user's
   own message is already in history, so the row is "this answer didn't arrive" plus **Retry**. A partial
   reply (deltas arrived, then the socket died) keeps what came and marks it unfinished: half an answer is
   worth more than none, and must not be mistaken for a whole one.
3. **Retry re-sends the user's message** on that chat - the agent client already has `retry()` for the same
   situation, so the shape exists.
4. **Distinguish a user-cancelled turn** (they pressed Stop) from a failed one: cancelled needs no row.

**Sean's rule, in one line: never silence.** Every turn ends in a reply, an error, or an interrupted row.

## Rig rows

- **C11** the mock **fails** a held turn after the window closes → on reopen the row reads interrupted and
  offers Retry. (Today's R4 only proves the SUCCESS handover; this is the row that was missing, and it is
  the same lesson as the vacuous passes: the failure path had nowhere to go and nothing asked.)
- **C12** Retry on that row re-sends and the reply lands in the same chat.
- **C13** a **partial** reply (deltas, then a cut) keeps the partial text and marks it unfinished.
- **C14** a turn the user **stopped** leaves no interrupted row.

## Item 4 corroborated by Sean

> *"after Sean closed and reopened the main window, selecting the Kick's Link now shows both slots, so the
> stale view only clears on reopen."*

That is exactly the diagnosis: the cache feeder is gated on the Chain tab, and the forced pass at
`PluginEditor.cpp:12024` runs on entering chain mode / a new editor. A reopen is the only thing that was
clearing it. It also means the K1 row must select the Link **without** reopening the window, or it would
pass on today's build.
