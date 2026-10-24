# Control through the live view — stages C and D

**Sean's ask, 24 Oct 2026, after the first live view came up clean:**

> *"Next, ahead of the slot meters: CONTROL through the view (your stages C and D). A change on the
> V2's live view goes to the real plugin on the Link straight away, so I hear it and the Link keeps
> it, and the view follows changes made on the Link itself. Short design note first: how parameter
> changes travel V2 → Link (and back), latency, what happens with plugins whose state isn't all
> parameters, and how it interacts with an agent run on that rack. Then build it."*

Design only. Nothing here is built. The read-only block shipped in the same build as this note
(`EJLiveViewInput.h`), and it is the thing this design replaces.

---

## 0. What already exists, and what does not

This is less new road than it looks, because the value ring was built for exactly this shape on
10 Oct and one of its commands has been sitting unused ever since.

**Built and proven:**

- `echojay::cmdring` (`Source/EJCmdRing.h`) — a 64-byte-per-frame, single-producer/single-consumer
  shared-memory ring, V2 writes and the Link drains in `processBlock`. Byte layout pinned by
  `static_assert`, offsets frozen, no allocation and no logging near the drain.
- Its rule for which channel a command belongs on, in its own words: *"DOES THE NEXT ONE SUPERSEDE
  THIS ONE? Yes → the ring. No → a file."* A knob drag supersedes; an `add` does not.
- Gesture framing already on the wire: `Phase { kBegin, kStream, kEnd }` and a `gesture` id every
  frame of one drag shares — *"the boundaries are what make ONE UNDO STEP PER GESTURE possible,
  which is why the coalescer may never discard them."*
- `Kind::kSetParam` — *"one parameter on one slot's device, in the device's own units (HUMAN GESTURE
  only)"* — with `slot`, `paramId` and the slot's `structureRevision` in `flags`, so a frame aimed
  at a rack that has since changed shape is discarded rather than mis-applied.
- The reverse direction, slowly: the rack sidecar publishes each slot's `settings` and
  `landedValues`, republished on change and at least every 10 s.

**Not built, and this is the honest gap:** the Link *drains* `kSetParam` and then throws it away.
`LinkProcessor.cpp:2929`:

> *"NOT APPLIED HERE, BY RULING. A device parameter is a plugin call and can never be made
> real-time safe, so it is staged for the message thread and the contract says so. Counted so a
> frame cannot vanish silently."*

It increments `ringSetParamPending_`, and the message thread's tick does nothing with it but log the
count. So the wire is finished, the gesture framing is finished, the discard-on-stale-structure rule
is finished, and **the last ten lines — applying it to the plugin — were never written.**

That is why this is a four-day job and not a two-week one.

---

## 1. V2 → Link: how a gesture travels

```
  the copy's own editor  (a guest NSView in the V2's card)
        |  the plugin writes to ITS OWN parameter
        v
  AudioProcessorParameter::Listener on the live copy          <- stage C, new (~80 lines)
        |  paramId = index into the slot's published schema-id list; value in the device's units
        v
  echojay::cmdring  kSetParam {slot, paramId, value, gesture, phase, structureRevision}
        |  shared memory, no lock, no allocation                <- BUILT
        v
  LinkProcessor::processBlock drains, stages by (slot, paramId) <- BUILT (it counts them today)
        |  one entry per parameter, last value wins
        v
  the Link's message-thread tick applies it to the real plugin  <- stage C, new (~60 lines)
        |  setParameterNotifyingHost / setValue on the slot's instance
        v
  heard, and published in the sidecar on the next republish
```

Three decisions worth stating, because each one is a fault family we have already paid for:

1. **The copy is never the authority.** A gesture moves the copy (the plugin does that itself — we
   cannot stop it, which is the whole reason the shield exists today), but the *value that counts* is
   the Link's. When the Link acknowledges, the copy is reconciled to what landed. If they disagree,
   the Link wins and the copy is corrected. One fact, one owner — the house's oldest rule, and the
   thing today's build gets wrong by having no owner at all.
2. **Coalesced by parameter, never by frame.** A 400-frame drag applies as the latest value per
   `(slot, paramId)` per tick, except that `kBegin` and `kEnd` are never dropped — they are what
   makes one drag one undo step. The ring's own coalescer already does this for the slot figures;
   `kSetParam` joins it.
3. **`paramId` is an index into the slot's published schema, carrying `structureRevision`.** A
   parameter identified by *name* would be the proxy-for-an-identity family; a bare index would
   mis-apply after a rack change. Both are already solved on this wire — the frame is discarded if
   the revision has moved, which is `drain`'s existing behaviour.

---

## 2. Link → V2: the view follows the Link

Today the only reverse channel is the sidecar's `settings` string, which is correct but up to 10 s
late and republished whole. For a view that is meant to *follow* the real plugin, that is not enough:
turn a knob on the Link and the picture would lag by seconds.

**Stage D, the proposal: a value-feedback block appended to the existing 30 Hz `LinkMeterFrame`
publish** — the same mechanism, the same seqlock, the same zero-gate the slot meters will use:

- a fixed-size block per slot: `structureRevision`, a count, and up to N `(paramId, value)` pairs
  for the slot **currently in view** (the Link already knows which slot is tapped — the live view
  asked it to tap that slot, and that request is the natural place to say "and publish this slot's
  values");
- only the tapped slot, so the cost is bounded and does not scale with rack size;
- the V2 applies them to the copy's parameters **while no gesture of his is in flight on that
  parameter** — a follow that fought a drag would be the dial-fight we have already had once.

Cost of N: 32 pairs is 264 bytes per frame per rack; with one tapped slot that is ~8 KB/s. Nothing.

**Where this is weakest, stated plainly:** some plugins do not report a parameter change at all when
their own interface moves something (a graph drag, a modal, a preset load). For those, the view will
follow only what the 10 s sidecar republish carries, and the fallback is the same honesty the dial
already uses — the card says what was last read back and when.

---

## 3. Latency, with the figures that exist

| hop | cost | where the number comes from |
|---|---|---|
| copy's editor → parameter listener | < 1 ms | in-process, message thread |
| V2 writes the ring → Link drains | one host block | the ring is drained at the top of `processBlock`: 512 frames at 48 kHz = **10.7 ms** |
| drain → the Link's message tick applies it | up to one tick | the Link's existing tick, and this is the figure to watch |
| applied → heard | the next block | 10.7 ms |

So **change-to-audible lands in the 25–50 ms band** if the apply tick runs at 30 Hz or better, which
is what the plan's §3 set as the target (*"change-to-audible under ~30 ms cannot come from"* the file
channel — hence the ring). The one new risk is the apply tick's own period: if it is slower than
~30 Hz, a drag will feel stepped even though every frame arrived. **This is the number the build must
measure and the guard must assert in time, not in counts** — a state-only test of a real-time control
is not a test (the 100 ms solo rule, already on file).

The feedback direction is 30 Hz by construction, i.e. ≤ 33 ms, which is below the threshold where a
following meter or knob reads as laggy.

---

## 4. Plugins whose state is not all parameters

This is the part that cannot be finessed, and it is why control through the view is **not** a
substitute for the Link's own window.

Three kinds of change a plugin's interface can make:

1. **A reported parameter change.** Travels as above. Most knobs on most plugins.
2. **A change the plugin makes to its own opaque state without reporting a parameter** — a
   filter-graph drag, an IR load, a modal dialog, a preset recall, anything behind `getStateInformation`
   only. The copy's state changes; **nothing travels**. The Link's plugin does not have it.
3. **A change that alters the slot's structure** (a preset that changes the parameter list, a
   channel-count change). The `structureRevision` already makes these safe — frames are discarded —
   but the change itself still does not travel.

**The ruling this needs from Sean.** For (2) and (3) there are only two honest options, and the first
one is cheap:

- **(a) Say so, per plugin, on the card.** When the live copy's `getStateInformation` blob changes
  without any parameter having been reported, the view says *"this change can't be sent — open
  <plugin> on the Link to make it there"* and offers the button that does exactly that. One line, no
  silent loss, which is his own standard. **~half a day.**
- **(b) Ship the whole state blob to the Link.** It works for everything, and it costs what the edit
  session costs: the blob can be megabytes, `setStateInformation` is a long call on the Link's message
  thread, it may re-instantiate the plugin's own view, and it cannot be done per gesture — only on a
  boundary like "apply". That is the edit session we already have, and it should stay the road for it
  rather than be duplicated here.

**Recommendation: (a), and the edit session remains the road for (b).** Control through the view
covers reported parameters, which is what a knob is; anything deeper goes through Edit, which is
what Edit is for.

---

## 5. An agent run on that rack

An agent run and a human gesture both end up writing to the same slot's parameters, which is exactly
the collision the ring was designed around — and the design is already in the wire:

- `kSetParam` is documented **`HUMAN GESTURE only`**. The agent's road is the chain-cmd v2 `editOps`
  file channel (*"one road to a Link's rack"*), which is acknowledged, undoable and recorded in the
  run. That separation is already law and this does not touch it.
- So the rule is simply **who may write, when**:
  - while an agent run holds that rack, a gesture on the live view is **refused with the reason**
    (*"an agent is working on this rack — stop it, or wait"*) — the same sentence family the panel
    already uses for a locked rack, and the same decision function pattern. It is not silently
    dropped, which is the fault he reported today.
  - while a human gesture is in flight (between `kBegin` and `kEnd`), the run's next `editOp` on
    that slot waits for the `kEnd` — a drag is a single step and must not be split by a run's write.
  - the run's own writes **publish back** through the feedback block of §2, so the view follows the
    agent as it works. That is a bonus, not extra work: it is the same channel.
- **Undo:** the run's steps are already undoable by identity (shipped this morning). A human gesture
  through the view must enter the same history as one step per gesture — which is precisely what the
  `kBegin`/`kEnd` framing exists for, and why the coalescer may not drop them.

---

## 6. Cost and time

| stage | what | time |
|---|---|---|
| C1 | the Link **applies** staged `kSetParam` on its message tick, to the real plugin, coalesced per `(slot, paramId)`, honouring `kBegin`/`kEnd` as one undo step | **1 day** |
| C2 | the V2 listens to the live copy's parameters and writes `kSetParam` with the gesture framing; the shield comes down with `kLiveViewControlBuilt` | **1 day** |
| C3 | the agent/human arbitration of §5 as one pure decision function, plus its refusal sentence on the card | **0.5 day** |
| D1 | the per-slot value-feedback block on the 30 Hz publish (tapped slot only), V2 reader, reconcile-unless-dragging | **1 day** |
| D2 | the "this change can't be sent" line of §4(a) | **0.5 day** |
| G | guards: a two-process `link_state_guard`-pattern leg that drives a gesture V2 → Link and asserts the value landed **within a stated wall-clock bound**; a leg that asserts one undo step per drag; a leg that asserts a refusal while a run holds the rack; a leg that asserts the follow does not fight a drag | **1 day** |

**Total ~5 days**, of which ~1 is guards. The slot meters (approved, ~2 days) share D1's publish
mechanism, so building D1 first makes the meters cheaper — they are the same block with different
fields.

**Order I would build it in:** C1 → C2 (he can hear it work after two days, with a bench guard), then
C3, then D1 + the meters together, then D2.

## 7. What I have NOT established

- The Link's message-tick period, which §3 says is the one number that decides whether a drag feels
  smooth. It must be measured before C1 is called done, not assumed.
- Whether a given guest editor reports parameter changes at all when it is a **copy** rather than the
  instance in the signal path. Nothing suggests it would differ, but it is a claim until a real
  plugin is driven on the bench — and the Tube-Tech is the obvious first subject, under the same
  supervised exception.
- Whether `setParameterNotifyingHost` on the Link's instance can stall its message thread for long
  enough to matter on a drag. The edit session's apply has never been measured per parameter.
