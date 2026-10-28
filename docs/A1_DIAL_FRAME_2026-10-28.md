# A1: the dial, as B's two contracts have it (28 Oct 2026, session A)

Sean's day list, item 8: *"the build plan in order: A1..."*, where A1 is remaining-work §2 — "the dial
as its own step; dialling a plugin already on the rack (slot dial); dial ranges per control;
`clip_drive` with its dial".

## Two of the four were already built, and the list is out of date

| §2 bullet | state | where |
|---|---|---|
| the dial as its own step on the card | **shipped 23 Oct** | `EJAgentRunView.h`: `pushDialStepFor`, the landed-first ordering, a dial whose add is not on this card still getting a row. Legs KP5–KP5d in `agent_client_guard`. |
| dial ranges per control, in the control's own units | **shipped 25 Oct** | `EJCalibLoop.h`: `boundsForParams` reads B's `bounds` per control, intersects a twin pair, refuses an empty intersection ("never a guessed range"), and `boundsStated`/`boundsUnit` ride the sidecar so a handover cannot put the −60..+12 dB default back. |
| dialling a plugin already on the rack | **not built** | this round |
| `clip_drive` with its dial | **not built** | no trace of the field in the plugin |

The first was reported "not done" in `docs/REPORT_2026-10-22_evening.md` and then built the next day
against his tests 3 and 4; the remaining-work doc was written from the older note. Worth saying
plainly rather than quietly ticking: **nothing was re-implemented**, and the two rows that prove them
were already green.

## What was actually missing: the dial FRAME

B's `CONTRACT_dial_step_2026-10-23.md` and `CONTRACT_dial_slot_2026-10-25.md` change who drives a
dial. Today an add carries `dial_on_playback` and the rack dials itself afterwards, silently — which
is why Sean's test 3 saw the closing arrive before the dial reported. Under the contracts a dial is a
**call**:

```json
{ "op": "dial", "of": "<the add's id>" | "slot": 1, "name": "UnFairchild",
  "step_text": "Setting the UnFairchild... about 1 dB",
  "dial_on_playback": { "...": "the same object the add carries" } }
```

and the client answers it when the dial lands, with figures.

### A1a (this round): the frame, on the open rack

- **`Source/EJDialFrame.h`** — the decisions, pure, so a guard can drive both directions of each:
  which slot (`of` → the slot the add **landed** on, never the one it asked for; `slot` → only if
  the plugin B named still stands there), what the step says while it waits, and **when it answers**.
- **The client intercepts it** beside the playback wait it behaves like (`EJAgentClient.cpp`), rather
  than sending it to the executor: answering the moment it is armed is the defect, not a detail.
- **The answer is the dial's own figures**, `{name, slot, outcome, figure_db, end, control, value}`,
  from `CalibLoop::dialReportVar()`. The three-way reached/over/short decision was inside
  `sayLineFilled` and is now `outcomeWordFor`, used by both — a sentence and a figure that could
  disagree about the same dial is the family three of this week's defects came from.
- **`skipped`** with B's own reasons when the subject is gone (`add_failed`, `slot_changed`, naming
  what it found), and **nothing is armed on the way to saying so**.
- **`not_heard`** at a 60 s bound, with the dial **left armed** for the next playback, as B asks.
- **`dial_follows: true`** on an add stops the add arming its own dial, at one point: the field is
  not copied onto the op. Nothing arms, so nothing can arm twice.
- **One arm for both roads**: `EJAgentHome::armDialAtSlotOnOwnRack` is the body an add's dial already
  used, split at the slot.
- Legs **DF0–DF11** in `agent_client_guard` (the ten pure ones both ways, then the whole road through
  the real client: armed-and-open, the "Play the track" line, the landed figures, the three refusals,
  and an ordinary `do` untouched).

### Settled with B: the gating is PER CHANNEL (28 Oct, 11:20)

I put the problem to B (`FOR_B_DIAL_FRAME_GATING_2026-10-28.md`) and B agreed:

> *"the server sends the dial-frame shape (add with `dial_follows`, then the dial frame) only to a
> channel that reports `dial_step` / `dial_slot` itself. For the V2's own rack that's the start
> context's capabilities. For a Link it's that Link's caps in `liveLinks`, which your `dialCapable`
> byte feeds. Every other channel gets today's road: the add carries its own dial, with no
> `dial_follows`."*

So nothing regresses at any point, an already-installed Link keeps dialling exactly as it does today,
and there is no flag day. B will not ship either dial branch with the per-client gate, and both
client-wide flags stay withheld until A1b, which then declares them **per Link**.

### The capabilities are withheld, deliberately

B gates both contracts on A's start context listing `"dial_step"` and `"dial_slot"`. This build has
half the road: the arm reaches the **open rack** only. A run aimed at a Link would answer "the dial
could not be armed" — and because `dial_follows` stops the add arming its own, that run would end
with **no dial at all**, a regression on a road that works today. A capability that is true of one
rack and false of the other is not what a capability says, so it waits for:

- **A1b** — the Link's own road for a dial naming a slot it did not just add, then both
  capabilities in one commit. **Its wire shape, decided 28 Oct from what the file already does:**
  the dial rides the chain-cmd as **its own top-level field**, `dialOnSlot: { slot, name,
  dial_on_playback }`, beside `editOps` — **not** as an op inside them. Three reasons, each from the
  code rather than from taste:
  1. **A dial is not an edit.** `ChainHost`'s op road ends in `bad("unknown operation \"" + op.op +
     "\"")`, so a `dial` op inside `editOps` would make an **old Link refuse the whole batch** — and
     a new one would have to carry a no-op edit for it. An unknown top-level FIELD is simply ignored
     by any JSON reader, old or new, which is the difference between additive and breaking.
  2. **The file already works this way, and says so.** `wantBusyAck` is the precedent, with the
     reason written beside it: *"ONLY FOR A SENDER THAT ASKED FOR IT, which is what keeps this
     additive … 'absent' means 'that sender cannot understand this answer'."*  So the Link's ack
     gains `dialArmed` (and `dialWhy` when false), and a V2 that gets neither knows it is talking to
     a Link without the road.
  3. **The answer comes back on the ack**, which is the only place the V2 already waits for a Link's
     verdict. `armDialAtSlot` for a Link is then: write the command, wait for the ack (the existing
     wait), read `dialArmed` — and the dial frame's `dial_not_armed` carries the Link's own sentence.

  **And it spends the LAST spare byte of `RegistrySlot`.** The per-Link capability B gates on has to
  come from the Link itself, and that is `LinkShm`'s registry slot: 128 bytes, with **`_pad[1]` at
  offset 127 — one byte left**, carrying the note *"one byte left; read byte 125's note before
  spending it"*. Byte 125 is `dialCapable`, and its note sets the rule for whoever carves the next
  one: *"keep the fallback the SAFE answer, not the convenient one. A zero here withholds a feature;
  a zero that meant 'capable' would hand payloads to binaries that drop them."*  So
  `dialFrameCapable` at 127 reads 0 from every existing Link, meaning **"cannot arm a dial on a named
  slot"** — which withholds the dial-frame shape for that channel and leaves B's old road in place.
  That is the safe direction, and it is the one B's per-channel gating needs.
  **After A1b there are no spare bytes**: the next per-Link capability needs a layout version bump,
  which is a decision for Sean rather than a line in a patch.

  **And the arm has to become asynchronous, which A1a's interface is not.** `armDialAtSlot` returns
  `bool` today, and that is honest for the open rack: `calibStart` is immediate. A Link's answer is
  not — it comes back on `chain-ack-<uid>.json`, and the V2's own road for that is
  `awaitChainAck (uid, seq, cb, maxWaitMs = 8000)`, **a callback**. Treating "the command was
  written" as "the dial was armed" is precisely the fault Sean's item 1 of 13 Oct is about (*"'sent'
  is not 'applied'"*), so A1b changes the interface to answer through a callback and the client's
  `beginDialStep` only starts its wait once the arm has answered. The step is already open at that
  point, so there is nowhere for the extra second to leak: it is the same wait, told honestly.

  The Link side is then the arming it already does, with the slot resolved by **which slot holds that
  name** rather than by `slotTheBatchAdded`, and without the `pushIsBuild` gate — which is exactly
  what makes it a dial on a slot nobody just added. Where that is today: `LoudnessLoop::dialsOnPlaybackFromOps` takes only
  `add`/`replace` ops (`if (op != "add" && op != "replace") continue;`), and `LinkProcessor`'s arming
  is additionally gated on `pushIsBuild` with the slot resolved by `slotTheBatchAdded` — a dial op on
  a slot the batch did not touch fails all three. So A1b is: a dial op recognised in that collector,
  the slot resolved by **which slot holds the name** (the same identity rule, different question),
  and the build gate not applied to it — plus the V2 side translating the frame into that op for a
  Link target, which is `ExecutorDo::translate`'s one new branch.
- **A1c** — `clip_drive` (`CONTRACT_clip_drive_2026-10-23.md`): the slot card's IN raised until the
  peaks clip 1–1.5 dB with OUT following it down, measured as the median peak reduction **across the
  slot** over the loud blocks, reported as `clip_drive` beside the dial. A measurement this plugin
  does not have yet, and the one new dial *kind*.

## A1c's measurement: what already exists, and the one gap

`clip_drive` asks for the **peak reduction across the slot**: the sample peak arriving before the IN
trim against the peak leaving after the OUT trim, in 100 ms blocks over the same stretch, the median
over the "loud" blocks (those within 2 dB of the loudest peak in).

The plugin already measures most of it. `ChainHost::SlotLevels` carries a `LevelTally::Snapshot` for
each slot's **in** and **out**, and a snapshot carries `hopTruePeak[]` — the true peak of each of the
**last 30 hops (100 ms blocks)**, with `hopTruePeakCount` saying how many are valid. So the pairing,
the 2 dB "loud" test and the median are arithmetic over two arrays this build already publishes.

**AND ONE THING THE CONTRACT ASSUMES THAT IS NOT TRUE HERE, found by reading rather than by a wrong
figure.** B reasons that *"the two trims cancel, so anything the clipper doesn't touch reads 0"*,
because its `peak in` is *"the sample peak of the signal arriving at the slot, BEFORE the IN trim"*.
In this plugin the slot's `in` tally is pushed by the **Slot Pre-Trim node after it has applied the
trim** — its own comment says so: *"the in tally, from what the plugin is about to hear"*. So with the
IN trim at +I and the clipper taking R off the peaks:

```
in  (after IN)  = L + I
out (after OUT) = L + I - R - I = L - R
in - out        = I + R        <-- not R
```

At B's own worked example (+5.5 of trim, 1.3 dB of clipping) the figure would read **6.8 dB** and stop
the dial on its first window — a true measurement of the wrong quantity, which is the worst kind. So
`EJClipDrive.h` takes the trim back out, by the dial that set it, and the figure **carries the trim it
removed** so the arithmetic can be checked from the answer alone. Legs CD0d–CD0f hold both
directions (6.8 with the trim left in, 1.3 with it taken back). The alternative — a second tap before
the trim — would be a new measurement for an arithmetic difference the dial already knows.

**The gap is the window, not the measurement**: 30 hops is 3 s and the contract wants *"at least 8 s
of playback and at least 10 loud-peak blocks before a figure counts"*. So A1c accumulates pairs
**across the dial's own ticks** rather than asking for a longer history — the calibration loop
already ticks per window and already owns "how much has been heard", which is where a bound like
this one belongs. Worth saying before the code: the figure must carry its window, which is decision
4 of `COMPARE_REFERENCE_PLAN` and the reason `macroAccumSeconds` exists beside `macroBandAccum`.

## One thing B asked A to look at, still open

`CONTRACT_dial_sides_and_words_2026-10-23.md` ends with it: at 08:43 on 23 Oct the Vac Attack's
Reduction ended at **+0.0** — the *lightest* end for a `higher_is_harder` control — while the measured
reduction (0.2 dB) was still **under** the 1 dB target. More reduction needed means a higher
Reduction, so either the dial moved the wrong way for that sense, or it read one side's meter while
both sides moved. Nothing in A1a touches the loop's stepping, so this is untouched and still owed; it
belongs with A1b, which is the next time that loop is opened. The leg it needs is the one the sense
rule has never had: a `higher_is_harder` control under target must step **up**, and a
`lower_is_harder` one must step **down**, from the same measurement.
