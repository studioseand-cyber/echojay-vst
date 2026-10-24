# Per-slot meters on a Link's slot cards

**Design only. No code.** Asked for overnight, 24→25 Oct 2026. Approved earlier as ~2 days.

What he asked for: **in, out and gain reduction on every slot card of a Link's rack, fed from the
Link at 30 Hz.**

## 1. The numbers already exist, and that is the whole reason this is cheap

The Link runs the same `ChainHost` the main plugin does, and its per-slot level tallies are **already
measured on the audio thread for every slot, third party included** — that is where the sidecar's
`inLoudestDb` / `inTypicalDb` / crest / true-peak fields come from today
(`EJRackSidecarFill.h`, from `host.getSlotLevels(i)`).

So this feature adds **no measurement**. It adds a faster road for numbers that are already taken.

Today's road is the sidecar: correct, but revision-gated and republished whole, so a card's figures
move when the rack changes or when a meter-driven republish happens — not at a meter's own rate.

## 2. The road: one small shm block, the shape layer 2 already proved

`shadowval_<uid>.bin` was built last night for one slot's parameter values. The meters want the same
shape for every slot:

```
  slotmeters_<uid>.bin     (new, fixed, mmap'd, seqlocked — one frame per slot)
    seq, slotCount, structureRevision
    per slot: inDb, outDb, grDb, truePeakDb, fieldsMask, frames
```

- written by the **Link's message thread** on its existing 30 Hz tick, from the tallies the audio
  thread has already filled;
- read by the **V2's message thread** on its 20 Hz editor tick;
- `fieldsMask` per slot, because a field that has no reading must be absent rather than zero;
- `structureRevision`, so a frame taken before a rack change is discarded rather than drawn onto the
  wrong cards.

**Cost of the frame:** 32 bytes per slot. Eight slots at 30 Hz is **7.7 KB/s**. Sixteen slots is 15
KB/s. It does not register.

## 3. What it costs on the audio thread

**Nothing new.** The tallies are already computed there for the sidecar; this changes only who reads
them and how often. The audio thread's work is identical with this feature on or off, which is the
same bar stage A set for the live view's tap ("the cost of the feature when it is not on screen is
zero, not 'small', zero").

The message-thread cost on the Link is one pass over N slots copying four floats each — a few
hundred nanoseconds — on a tick that already publishes a meter frame.

## 4. Gain reduction, and the line I will not cross

**EchoJay does not read a third party's gain-reduction meter and does not estimate one.** That is
Sean's ruling of 18 Oct and it stands.

What can be shown per slot:

- **in** and **out**: measured, ours, honest.
- **the difference across the slot** (in − out): this is what the live view's copy already reports,
  and it is *not* the plugin's GR. On a compressor doing nothing but making up gain it reads
  negative; on one with its output trimmed it reads large with no compression at all.

So the card shows **in**, **out**, and the difference **labelled as the difference across the slot**,
never as "GR" — unless the slot holds an EchoJay device, which reports its own and may be labelled
GR because it is one. One field, two provenances, and the label says which.

## 5. How it fails safe

1. **A frame nobody has published reads as NO READING, not as 0.0 dB.** `LinkMeterFrame`'s own
   comment states the rule — *"an ungated dB field reading 0.0f would render as a reading"* — and
   this block keeps it with `fieldsMask`. A card with no frame shows the dash it shows today.
2. **A stale seq is no reading, not the last reading held for ever.** The V2 compares the sequence;
   unchanged for longer than a stated age means the Link is not publishing, and the cards go to
   dashes rather than freezing on a number that was true a minute ago.
3. **Zero frames measured means NOTHING WAS MEASURED, never "silence"** — the GR window's rule, and
   the distinction the rig's B9 could not make for twenty-five seconds.
4. **A rack that changes shape under a frame discards it** (`structureRevision`), so a figure never
   lands on a card it does not belong to.
5. **An older Link publishes no file**, and that is the correct downgrade: the cards keep the
   sidecar's slower figures, exactly as they look today, and nothing says the meters are live when
   they are not.

## 6. Cost

| piece | time |
|---|---|
| the block, the Link's publisher, the V2's reader, the masks and the staleness rule | 1 day |
| the cards: three figures per slot, the labelling rule of §4, the dash states | 0.5 day |
| guards: an unpublished field is absent and not zero; a stale seq is no reading; a structure change discards; the difference is never labelled GR for a third party | 0.5 day |
| **total** | **~2 days** |

## 7. Why this before layer 3

Layer 3 insures against a fault **nobody has reported yet** — no plugin in his rack has been shown to
change itself without reporting a parameter. The meters answer a question he asks in every session.
If one of the two is built next, it should be this one.
