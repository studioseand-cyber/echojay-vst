# The Link as the processor, the V2 as a remote control

**Status:** plan only, written 8 October 2026 from a read-only pass over the shipped code and the 8 October logs.
Nothing in here is built. Every claim that comes from the code names the file; every claim that comes from a log
quotes it. Where I could not establish something I say so rather than guessing, because the whole point of this
document is to be safe to build from tomorrow.

---

## 0. The problem, in Sean's words and in the code's

A KICK track carries a Link. Logic routes it to a drum BUS. Editing the kick's rack from the V2 **leases** it: the
V2 hosts and processes those plugins, and the Link bypasses its own. So the kick's processed audio leaves the
graph **at the V2's position**, skipping the kick channel's fader, pan and sends, and skipping the drum bus and
everything on it. Sean is balancing a kick that is not going through its bus.

**The target:** the Link always processes its own rack, in place. The V2 is a remote control. Audio always follows
the DAW's routing, because the DAW's routing is the only thing that knows about faders, sends and busses.

---

## 1. Where leased audio is processed today — confirmed

`LinkProcessor::rackLeaseEngage()` (LinkProcessor.cpp:2549) does three things when the V2 takes a rack:

1. `chainHost.setAttachBypassed(true)` — from here every slot's **effective** bypass is forced true;
2. for every slot, `setLeaseBypass(i, true)` — the whole rack goes dry, with the user's INTENT saved separately;
3. `rackLeaseActive_ = true`, and the log says it plainly: *"RACK engaged … all bypassed, streaming dry"*.

So **during a lease the Link's own output is its input** — the rack is switched off in place. Meanwhile the Link
keeps writing its audio into the shared ring (LinkProcessor.cpp, the ring block after `applyGainSmoothed`), and
its comment states the intent: *"ALSO while an edit lease is held … the whole point of the lease is that the main
plugin is monitoring this channel through its editing copy."*

On the V2 side that ring is read and mixed into the V2's own buffer (`borrowCtxMix_`, PluginProcessor.cpp
1359-1406), and the borrowed rack is hosted by a second `ChainHost` — the "borrowed host" (`borrowHost()`,
`borrowHostIfActiveFor(uid)`).

**What the kick channel and the drum bus carry during a lease:**

| point in the graph | today, during a lease |
|---|---|
| kick channel, at the Link | the kick, **unprocessed** (rack dry) |
| kick channel fader / pan / sends | the kick, unprocessed |
| drum bus and its chain | the kick, unprocessed |
| the V2's position (e.g. the mix bus) | the kick **processed by the borrowed rack**, summed in |

That table is the bug. Nothing about it is accidental — it is what the lease was designed to do — but it means the
processed kick bypasses its own channel strip and its bus.

---

## 2. Every V2 rack operation, and whether it can become a Link command

The transport already exists: a per-rack command file (`chain-cmd-<uid>.json`) written by
`EchoJayProcessor::writeChainEditCommand`, read and applied by the Link through `applyChainEdits` /
`applyStructurePlanAndSync`, with an **ack** (status ok / stale / refused) and a base-revision staleness guard.
The ops vocabulary is already rich: `add`, `remove`, `replace`, `move`, `bypass`, `set`, `set_wet`,
`reset_levels`, `level_match`, `headroom` (ChainHost.cpp:2069-2139).

| V2 rack operation | maps to | state today |
|---|---|---|
| add a plugin | `add` op + structure plan | **exists**, acked |
| remove | `remove` | **exists** |
| reorder | `move` | **exists** |
| replace | `replace` | **exists** |
| bypass a slot | `bypass` | **exists** |
| slot wet/dry | `set_wet` | **exists** |
| dial a param / apply a proposal | `set` + `settings_structured` | **exists** |
| slot IN (pre-trim) and OUT gain | — | **MISSING as an op.** Today these are written straight onto the host (`setSlotPreTrimDb` / `setSlotOutGainDb`); item 5 wired the Link's own cards to its own host, but there is no transport op, which is why the V2's readouts on a REMOTE rack are read-only (`onSlotGainSet` is suppressed when `remote`) |
| master wet | — | **MISSING as an op** (local only today) |
| pre-chain gain | `setPreGainDb` | **exists on the Link's own side**; no op |
| level landing (the loop's writes) | `set` on the Level slot | **exists** as an op shape; the loop itself runs where the rack is |
| Save / Open a chain | no op needed | **client-side**: it serialises whichever host holds the rack — CHAINS (C) made that the view's rack |
| open a plugin editor | — | **MISSING**: see §4 |
| undo / redo | `{"op":"undo"}` | **exists** over the transport, answered by the receiving rack's own stack |

**What cannot map, and must be solved rather than translated:**

- **Opening a third-party editor.** An editor belongs to the instance that owns it. The V2 cannot show a window
  for a plugin living in another process without either an in-process path (§4) or the Link opening its own
  window.
- **Meters and readings.** These must come FROM the Link, not be recomputed: it is the only host that sees the
  audio. That is already how the mixer strips work (`LinkMeterFrame`), and item 4 made the V2's display of them
  honest.
- **Anything that needs a reply in the same gesture** (a readback after a dial, a dry-run before an apply) needs
  the latency work in §3 or it will feel broken.

---

## 3. Real-time feel: what the channel costs today, and what ~30 ms needs

**Today's command channel is a FILE, polled.** That is fine for "apply this edit" and hopeless for a knob drag:

- the lease gate polls on the Link's timer and the rack arm has a **3.5 s floor** before a command is accepted
  (the `pumpMs(5000)` wait in `lease_id_guard` exists precisely because of it);
- the apply ack is waited for with a **5 s** budget (the "10 s ack wait stays at 5 s" ruling);
- `EJStruct: close-apply FAILED … 2 edit(s) unacked in 5 s` in today's logs is that path timing out.

**I have not measured a single-command round trip** — that needs a build, and tonight is read-only. What I can
say from the code is that it is bounded below by the Link's poll interval plus a file write and a file read, which
is tens of milliseconds at best and seconds in the arm case. **Change-to-audible under ~30 ms cannot come from
this channel.**

**What it needs instead**, and the precedent is already in the tree: the meter frames and the audio ring are
**shared memory** (`LinkShm`, a registry of 128-byte slots at a 64-byte-aligned stride, proven byte-for-byte by
`shm_layout_guard`). A parameter-stream path should be the same kind of thing:

- a lock-free **command ring in shared memory**, one per rack, written by the V2 and drained by the Link on its
  own timer and/or at the top of `processBlock`;
- **coalescing by (slot, param)**: a drag writes the latest value, not every pixel — the reader takes the newest
  and discards the rest, which is what makes a drag cheap no matter how fast the mouse moves;
- **gesture framing**: `begin` / `stream` / `end`, so the Link knows when a gesture ends (that is also what makes
  one undo step per gesture possible — §9);
- the **file channel stays** for structure (add/remove/replace/move) and for anything needing an ack and a
  staleness guard. Two channels with different jobs: a ring for values, a file for decisions.

Under that design the floor is the Link's drain interval. Draining in `processBlock` makes it one buffer
(~2.7 ms at 128 samples), which is comfortably inside 30 ms; draining on a 20 Hz timer would not be.

---

## 4. Editors: do the V2 and the Link share a process?

**In Logic, today, yes — and I have evidence rather than an opinion.** Every one of the six
`AUHostingServiceXPC_arrow` crash reports from 8 October lists **both** `EchoJay V2` and `EchoJay Link` in
`usedImages`, in one process, alongside the third-party plugins. Logic hosts AU **views** in that service, and
both plugins' UIs were in it.

That is a fact about Logic's AU hosting, not a guarantee:

- **AUv3 / sandboxed hosts** put each plugin in its own container;
- **Bitwig** sandboxes by default (per plugin or per vendor, by setting);
- **Reaper** can bridge selectively (32/64, or "separate process" per plugin);
- **Pro Tools (AAX)** runs in-process but with its own rules.

**Recommendation.** Default to a **floating window owned by the Link**: the V2 asks the Link to open the editor
for slot N, the Link opens its own window, and the knobs are the real instance's, so a turn is instant by
construction. It works in every host and needs no shared address space. The code already knows how to make this
decision per plugin — `ChainHost::editorPlacement(...) == EditorPlacement::Float` is consulted by both editors
today (LinkEditor.h:765, PluginEditor.h's matching note).

**Fallback (the nicer one, where it is provably safe):** embed the Link's editor component in the V2's panel
**only** when both are in one process AND we can prove it at runtime — the registry already gives us a place to
publish a process id per Link, so "same pid" is a cheap, honest test. Never infer it from the host's name.

---

## 5. Discovery and the channel, across hosts

**Today:** a Link publishes itself into a **shared registry file** in a resolved directory (`LinkShm::resolveDir`,
`RegistrySlot` rows, claimed/released by `claimRegistrySlot` / `releaseRegistrySlot`), plus a **rack sidecar**
(`rack-<uid>.json`) and a **meter frame** per rack. The V2 reads those. There is no socket and no in-process
singleton: it is files plus shared memory in a known directory.

**That is the right foundation for this work**, because it is already cross-process. The risks are not about
hosts' plugin formats but about **sandboxes and file access**:

| host | expected | note |
|---|---|---|
| Logic (AU) | works today | both plugins in one view service; file + shm path already in use |
| Pro Tools (AAX) | works | same path; AAX is in-process |
| Reaper (VST3/AU) | works; **bridging** puts plugins in separate processes, which this design already assumes | prefetch: Reaper may render non-selected tracks ahead, see below |
| Ableton Live (VST3/AU) | expected to work | no hard sandbox |
| Cubase / Studio One (VST3) | expected to work | |
| Bitwig (VST3) | **at risk**: sandboxing may deny the shared directory | must be tested; if denied, no channel exists and the UI must say so rather than appear dead |
| AUv3 (iOS-style containers) | **will not work** as designed | hard container boundary; out of scope and must be declared so |

**Prefetch / lookahead, and why it matters here.** Logic and Reaper may process a track ahead of the playhead or
out of lockstep with other tracks (Logic's track prefetch on non-selected tracks; Reaper's anticipative FX). Two
consequences: a Link's meter frame can describe audio the user has not heard yet, and two Links' frames are not
guaranteed to be from the same instant. **Nothing in this plan may assume cross-track sample alignment.** The
same rule the loudness work already follows applies: a figure is only evidence with its own clock attached.

---

## 6. Removing the lease — what goes, and which bugs go with it

If the Link always processes its own rack, selecting a Link in the V2 stops being an audio act and becomes
**"which Link am I addressing"**. Then all of this can go:

- `LinkProcessor::rackLeaseEngage()` / `rackLeaseRelease()` and `rackLeasePrior_`;
- `ChainHost::setAttachBypassed` / `attachBypassed_`, and `setLeaseBypass` with it;
- the `intendedBypassed` **overlay** (the field can stay as the user's intent, but the two-state dance stops);
- the rack arm of `leaseGate_` (Engage/Expire/Release) and `leaseBaseRev_`, `rackLeaseMuteWant_`,
  `rackLeaseEditPending_`, `slotWetParkedSaid_`;
- the **borrowed host** entirely: `borrowHost()`, `borrowHostIfActiveFor`, `borrowEngageBegin`,
  `borrowRebaseAfterPush`, `clearBorrowKept`, `restoreBorrowDial`, `borrowCtxMix_` and the ring injection in
  `processBlock`;
- every "the Link parks its slots while leased, so the content lives in the borrowed host" special case — the
  edit path's divert, CHAINS (C)'s host choice, `fillRackSidecarSlots`' two publishers.

**Bugs it retires outright** (all of them real, all from the last two days):

1. the **persistent bypass after a hand-back** (08b item 0) — no lease, no snapshot, nothing to restore wrongly;
2. the **bypass DISPLAY** bug (every slot reading bypassed on a Link rack) — the effective state stops being a
   lease artefact;
3. **CHAINS (C)**'s wrong-rack Save — there is one host per rack, so there is nothing to choose between;
4. `lease_id_guard`'s whole subject, and its flaky 3.5 s precondition;
5. the parked-rack notices, the `leaseSlot0_` slot arm, and the "rack has 0 slots" refusals that came from a Link
   whose slots were parked;
6. the two-publisher sidecar, and with it the class of bug where the Link and the lease holder describe the same
   rack differently.

**What must be built to replace it:** the IN/OUT and master-wet ops (§2), the parameter ring (§3), the editor
request (§4), and the undo plumbing (§9). That is the honest trade: a smaller, simpler audio path in exchange for
a richer command surface.

---

## 7. Is "V2 processes and returns" viable? No.

The alternative — the V2 keeps processing the borrowed rack and sends the audio BACK to the kick channel — fails
on timing and on routing:

- **Thread and order.** The kick's Link and the V2 are different nodes in the host's graph, often on different
  threads, with no guaranteed order within a buffer. Returning audio to a node that has already run means a full
  buffer of delay at best, and a race at worst.
- **Latency reporting.** A round trip through another plugin's position cannot be declared to the host, so PDC
  cannot compensate it. The kick would sit late against everything else.
- **It does not even fix the bug.** The processed kick would still have been computed at the V2's position; to get
  it back in front of the kick's own fader you would have to return it *upstream of yourself*, which the graph
  does not allow.
- **Prefetch** (§5) makes any cross-track return unsound in Logic and Reaper regardless.

So: the rack must run where the rack lives. That is the whole premise of this plan.

---

## 8. Build plan

**Stage 1 — stop the audio detour (smallest safe change, and it fixes Sean's kick on its own).**
Keep the lease as a *control* concept, but stop bypassing the Link and stop processing in the V2: the Link keeps
processing its own rack, and the V2's borrowed host becomes **display + edit staging only**.
*Exists:* the command channel, acks, structure plans, the sidecar, the meter frames.
*Missing:* nothing new — this is subtraction. `rackLeaseEngage` stops bypassing; the ring injection stops mixing.
*Effort:* small. *Test:* the kick through the drum bus — solo the bus, confirm the kick arrives processed and that
the kick's fader and sends move it; confirm the V2's position adds nothing.
*Risk:* low, and the failure mode is audible immediately (double processing if the injection is not also removed).

**Stage 2 — the ops that are missing (§2).** Slot IN/OUT, master wet, pre-gain as transport ops with acks, so the
V2's own controls work on a remote rack instead of being read-only.
*Effort:* medium. *Test:* each control moved from the V2 changes the Link's own figure and comes back in its
sidecar; a refused op says so.
*Risk:* low-medium — every new op needs its staleness guard.

**Stage 3 — the parameter ring (§3).** Shared-memory ring, coalesced, gesture-framed; drained in the Link's
`processBlock`.
*Effort:* medium-large; this is the only genuinely new mechanism. *Test:* a drag's end-to-end latency measured
with a click track; a dropped-frame test (fill the ring and confirm the newest value wins and nothing is applied
twice).
*Risk:* medium — it is lock-free code on the audio thread, so it gets the same discipline as `LinkShm`: a layout
guard with a golden image, and no allocation anywhere near the drain.

**Stage 4 — editors (§4).** The Link opens its own floating window on request; the same-pid embed as a later
refinement.
*Effort:* medium. *Test:* a third-party editor opened from the V2 responds instantly to its own knobs; closing it
from either side is clean (and the FG-X 2 finding says: never tie the LAST view's lifetime to our own teardown).
*Risk:* medium — window ownership across processes is where the AU view service's own crashes live.

**Stage 5 — undo (§9) and the removals (§6).** Only after 1-4 are green, because the removals delete the paths the
earlier stages are still being compared against.
*Effort:* medium. *Risk:* low by then.

**Saved projects made under the old behaviour.** A project saved mid-lease today holds: the Link's own model with
its slots' INTENT (08b item 0 made that true), a V2-side borrowed copy, and a lease file. After the change,
`rackLeaseActive_` never becomes true, so the restore path must treat an old lease file as **expired on sight**
(the gate already has an Expire arm — keep it for exactly this), and the Link's model restores its intent, which
is the user's rack. **The borrowed copy in a V2 state becomes orphaned data and must be ignored, not applied** —
if it were applied we would duplicate the rack. That is one explicit migration test: open a project saved during
a lease and confirm the Link has its rack, the V2 shows it, and nothing is doubled.

---

## 9. Undo under remote control (08c item A, and it belongs here)

**Today EchoJay's own undo records nothing that happens on a Link.** The stack is
`EchoJayProcessor::undoHistory_`, wired by `wireUndoHooks(chainHost, {})` — i.e. to **ChainHost mutations in this
process** (the local rack and the borrowed copy): add, remove, move, bypass, wet, keep-level, one step per
`applyChainEdits` batch, plus the loop's level writes via `loudnessLoop_.onGainWritten`. A Link Mixer fader move
is a **transport command**; no ChainHost on this side mutates, so no hook fires and nothing is recorded. Undo is
not failing — it was never told.

**Everything that skips the stack today**, by the same rule: the Link Mixer's gain, mute, solo and pan; a slot
bypass or wet sent to a Link; dials applied to a Link slot; a chain edit or proposal Apply diverted to a Link; the
Link's pre-gain.

**Under remote control this is not a nicety — it is most of the product**, because then *every* rack edit is a
command to a Link. The design:

- **one step per GESTURE**, pushed on **release** (the wet knob's existing "one step per knob gesture" is the
  precedent), carrying `{uid, what, slot, before, after}`;
- **undo SENDS the previous value back** as an ordinary command and **waits for its ack**; an undo that did not
  land says so rather than silently leaving the stack and the rack disagreeing;
- the entry is a **CHAIN entry in the plugin-wide history**, the same class the local rack uses, so one Undo
  button walks local and remote edits in one order;
- the gesture framing in §3 is what makes "one step per gesture" possible at all — without `begin`/`end` the V2
  cannot tell a drag from a hundred writes.

**Not Logic's header Undo.** That records host parameter gestures on the instance owning the parameter, and a
Link Mixer move is not a host parameter on the V2. Making these host-visible would put every Link's gain into the
V2's parameter list — a much larger decision, with automation and preset consequences, and it is not needed to
make Undo work.

---

## 10. What I have NOT established

- **The actual round-trip latency of one command** (§3). It needs a build and a measurement; the estimate above is
  from the code's structure and today's 5 s ack timeouts, not from a stopwatch.
- **Whether Bitwig's sandbox permits the shared directory.** Must be tested on the machine, not reasoned about.
- **Whether the same-pid embed is safe in Logic in practice** (§4) — the six AU-view-service crashes of 8 October
  are all third-party exit handlers, but they are a reminder that this process is not ours and its lifetime is
  not under our control.
