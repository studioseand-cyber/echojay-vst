# Next round — the queue, in order

Rewritten 11 Oct 2026, after the round that closed item 1 (plan §6's removals) except the mirror object.
Nothing here is started.

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
