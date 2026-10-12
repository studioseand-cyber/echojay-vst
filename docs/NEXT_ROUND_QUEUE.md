# Next round — the queue, in order

Rewritten 12 Oct 2026, after the round that took Sean's items 1-11.

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

## 0b. buildNotes in the local result bubble — offered, not taken

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
