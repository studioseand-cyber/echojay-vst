# Plan: what a Link publishes about itself

**Sean's next round, 20 Oct 2026. Design first, build on his word.**

- **(a)** Per-slot gain reduction on the rack sidecar, so the agent can read an EchoJay device's GR,
  and a certified plugin's where it can.
- **(b)** A source type for each Link: the same type setting the V2 has, guessed from the track name
  (and B's classifier if that is cheap), changeable by Sean on the Link, sent as `channelRole` on
  both roads.

---

## 0. A correction before the plan

I told him (a) needed "a new per-slot field on the rack sidecar … a sidecar layout change". **That was
wrong, and in his favour.** Two things I had not checked:

1. **The sidecar is JSON, not a mmap'd struct.** `writeRackSidecar` / `readRackSidecar` compose and
   parse with `juce::DynamicObject` and `hasProperty` guards, so adding a key is additive and cheap.
   `v` stays 1 — the file's own rule, "additive means additive". The positional-brace-init trap the
   file records five times is about the **C++ `RackSidecarSlot` struct**, which is brace-initialised
   in `LinkProcessor::publishRackSidecar`: a new C++ member must go **last** and be set by
   assignment. That is a discipline, not a layout risk.
2. **The per-slot GR fields are already there, and already published.** Added 18 Oct as *"ITEM 13:
   THE LINK'S OWN MEASUREMENTS"*, with exactly the conventions he has ruled:
   `pictureValid, grKnown, grCertified, grDb, grPeakDb, grTypicalDb, grLoudestDb, grHeldDb,
   grWindowFrames` — and the field comment already states *"grWindowFrames == 0 means NOTHING WAS
   MEASURED, never 'no reduction'"* and *"grCertified is a property of the PLUGIN … the 'no estimates,
   ever' ruling turns on it."* The publish digest moves on them (`pic.grHeldDb * 7.0 +
   pic.grLoudestDb * 13.0 + pic.grWindowFrames`), so a Link republishes while it is measuring.

**So (a) is not a publisher problem at all. It is two consumer problems**, and one of them is not
about Links:

- **`slot_levels` has no GR field of any kind.** `EJSlotLevelsBlock.h` carries levels, crests, a true
  peak and `readBack` — and no gain reduction. So "how much gain reduction is it doing?" could not be
  answered from that block **even on the V2's own rack**.
- **`slot_levels` is composed from the wrong rack for a Link target.** All three call sites pass
  `processorRef.getChainHost()`, the V2's own rack. On a turn aimed at a Link the V2 hosts nothing,
  every row is dropped, and the block is omitted — which is exactly what his 16:03 body shows (no
  `slot_levels` key at all).

That makes (a) smaller than I said, and it should ride the next install rather than own a round.

---

## (a) Per-slot GR, to both roads

### (a1) The block learns gain reduction — `Source/EJSlotLevelsBlock.h`

Added to each row, from `host.slotPicture(i)`, **measured or absent** by the same rule the rest of the
block keeps (a figure whose window has not filled is omitted, never sent as 0):

| Field | Source | Meaning |
|---|---|---|
| `grLoudestDb` | `pic.grLoudestDb` | reduction on the loud phrases (95th percentile, his ruling) |
| `grTypicalDb` | `pic.grTypicalDb` | the median |
| `grHeldDb` | `pic.grHeldDb` | the 2 s peak-hold figure |
| `grPeakDb` | `pic.grPeakDb` | the deepest in the window |
| `grNowDb` | `pic.grDb` | the live needle |
| `grCertified` | `pic.grCertified` | **the plugin** is certified, not "audio was heard" |
| `grWindowSeconds` | `pic.grWindowFrames / sampleRate` | **0 frames = nothing measured**, so the row omits every GR field |

Two rules carried over verbatim, because they are the ones that have bitten:

- **`grWindowFrames == 0` omits the whole GR group.** A floor (−200) printed as a figure is the
  failure mode this convention exists to stop.
- **No estimates, ever.** `grCertified` false means EchoJay does not read that plugin's meter, so no
  GR fields are sent and `grReadable: false` says why in one word. A third party's GR is absent, not
  guessed — and the Link's own log already says this out loud (*"a third-party plugin's is not
  measured by this build, so check(gr) on one answers not_certified"*).
- An EchoJay device always qualifies, including the **4-Band (multiband)** since the 12:02 override;
  its figure is the deepest non-bypassed band.

### (a2) The block can be built from a sidecar — the same function, a second source

`slotLevelsBlock` takes `const ChainHost&` and nothing else, deliberately ("two composers of one
measurement is how the chat and the card come to disagree"). That rule is kept by giving the row
builder **one input struct** and two adapters:

```
struct SlotLevelRow { int slot; String name; ... all the figures ..., String readBack; };
rowsFromChainHost (const ChainHost&)            -> std::vector<SlotLevelRow>   // the open rack
rowsFromSidecar   (const LinkShm::RackSidecar&) -> std::vector<SlotLevelRow>   // a Link
slotLevelsBlock   (const std::vector<SlotLevelRow>&) -> juce::var              // ONE composer
```

Every figure the sidecar needs is already on it: `inRmsKnown / inLoudestRmsDb / inTypicalRmsDb /
inRmsSeconds`, `outRmsKnown / outLoudestRmsDb / outTypicalRmsDb`, `inCrestKnown / outCrestKnown /
inCrestDb / outCrestDb`, `inLoudestPeakDbtp`, the GR group above, `landedValues` and `dialConfirmed`.
**No new sidecar field is needed for (a).**

### (a3) The call sites choose by target

The three `api.setSlotLevelsVar(...)` sites become: the Link's sidecar when the turn is aimed at a
Link (`readLinkRackSidecar(effectiveChannelUid())` — the same resolver `channelRole` and
`channelWidth` already use, so all three describe one target), the open rack otherwise. The agent road
gets the same rows through the executor, so both modes answer the question identically.

### (a4) Guards

`level_slot_guard` already owns this block (legs SL92e–h). New legs: GR present for a certified
device; **every GR field absent when `grWindowFrames == 0`** (the RED-first one); absent for an
uncertified plugin with `grReadable: false`; a sidecar-built row equal field-for-field to a
ChainHost-built row from the same figures (that is the only assertion that can stop the two sources
drifting); and the 4-Band reporting its deepest band.

### (a5) Cost

Half a day, and it is a QUICK round: no sidecar layout change, no audio-thread code, one new composer
with two adapters and five legs.

---

## (b) A source type for each Link

### What exists, and what is missing

- The V2 has `ChannelType` (`channelTypeNames[]`, plus `getCustomChannelName()` for `Other`) and
  `kindOf()` collapsing it to `channel | bus | mix_bus | master`.
- A Link has **no source type at all**. Its registry row carries a **placement** (0 unset, 1 bus,
  2 insert, 3 send) and that is a different fact: where it sits, not what it is.
- So "Nafe Main_01 is a vocal" was never knowable on either road. Agent-off's `channelRole` was
  literally `{"kind":"channel","name":"Nafe Main_01"}`. **B was not missing a field; the fact did not
  exist.**

### (b1) Where it lives

**On the sidecar, and not in the registry.** The registry row has **exactly one free byte** (`_pad[1]`
at offset 127, with a note saying to read byte 125's rules before spending it), and a one-byte enum
would fit — but the V2 already reads every Link's sidecar at ~1 Hz and caches it, so the registry buys
nothing and the last byte stays unspent for something that genuinely needs the heartbeat path. If it
ever does need to be in the registry, that is the moment to bump `kRegLayoutVersion`, not now.

```
sourceType      "lead_vocal" | "drums" | "bass" | ...   (the ChannelType names, snake_cased)
sourceTypeFrom  "user" | "name" | "classifier"          WHERE IT CAME FROM
sourceTypeName  the custom name, when sourceType is "other"
```

Additive JSON keys at `v:1`; a new C++ member goes **last** in `RackSidecarSlot`'s parent
(`RackSidecar`) and is set by assignment, per the brace-init rule. An older reader never asks; an
older writer's silence reads as "this Link does not state its source", which is **not** the same
sentence as "this Link has no source type".

### (b2) `sourceTypeFrom` is the point of the design

A guess and a decision must not be the same value. The three provenances rank, and a lower one never
overwrites a higher one:

1. **`user`** — Sean set it on the Link. Wins always, survives a rename, and is the only one that
   persists in `getStateInformation`.
2. **`classifier`** — B's answer for this track name, cached by name.
3. **`name`** — the local guess.

And the fourth state is **absent**: the guess was not confident. A wrong type is worse than none —
B picking a vocal chain for a drum bus is a worse failure than B asking — so the guesser must be able
to decline, and `channelRole` then carries kind and name exactly as it does today.

### (b3) The name guess

A pattern table, case- and separator-insensitive, matched on whole words against the Link's display
name with the host's numbering stripped (`_1_01`, `-2`, trailing digits): `vox|vocal|lead|adlib|
ad-lib|bv|backing` → vocal roles; `kick|snare|hat|oh|room|drum|perc|tom` → drums; `bass|808|sub`;
`gtr|guitar|keys|piano|synth|pad|str|strings|brass`; `mix|master|2bus|stereo out` → the bus kinds.

Three rules that keep it honest:

- **Whole words only.** "Bassoon" is not a bass and "Vocoder" is not a vocal; substring matching is
  how a guesser earns distrust.
- **Two different families matching = no guess.** "Vocal Drums Buss" declines rather than picking.
- **His own names are the test material.** `Nafe Main_01`, `Nafe Verse Adlibs_1_01`,
  `Nafe 2 Auto Main_1_01`, `88_7even Kick_2_01`: "Adlibs" and "Kick" resolve; **"Main" does not** —
  it could be a bus or a lead vocal, so it declines, which is the correct answer and the one the leg
  asserts. That is also why (b) cannot be only a guesser: most of his tracks will need the setting.

### (b4) B's classifier, and whether it is cheap

Cheap **only if it rides a request already being made** — the agent `start` body or a chat turn, as
one extra array of unresolved track names, answered in the same response and cached by name. It is
**not** cheap as a call per Link: his session shows 30+ Links, and a request per Link on load is a
burst of calls for a field nobody has asked for yet. So: never on load, never on a timer, and never
for a name that already has a `user` value. If B would rather not carry it, the local guess plus the
setting is enough — the classifier is an improvement to the guess, not the mechanism.

### (b5) The setting, on the Link

The same control the V2 has, in the Link's own window beside its placement selector: a dropdown of
`channelTypeNames` plus "Other" with a text field. Setting it writes `sourceType` +
`sourceTypeFrom: "user"`, persists in the Link's state, and republishes the sidecar immediately
(a gesture, not a meter — it does not wait for the digest). Unset shows the guess, dimmed, with its
provenance, so he can see *why* the agent thinks a track is a vocal and correct it in one click. Only
the UNSET state is dim, never amber — the Link editor's existing rule.

### (b6) `channelRole` on both roads

One composer, `echojay::channelRoleVar(target)`, returning:

```json
{"kind":"channel","name":"Nafe Verse Adlibs_1_01","sourceType":"backing_vocal",
 "sourceTypeFrom":"name","placement":"insert"}
```

- `kind`, `name` and `placement` exactly as today, so B's existing reader keeps working.
- `sourceType` / `sourceTypeFrom` **absent** when unknown.
- Called by `chatTargetChainRole()` (agent off) and `ExecutorRead::startContext()` (agent on), which
  since this afternoon's round already send the same two keys. One composer means they cannot drift.

### (b7) Guards

`overlay_gate_harness` or a new `source_type_guard`: the guesser on **his real track names**, with
"Main" asserted to DECLINE; whole-word matching (bassoon/vocoder); two families → no guess;
provenance never downgraded (a rename does not overwrite a `user` value); `channelRole` identical
from both roads for one target (the drift assertion); and absent-not-guessed on the wire.

### (b8) Cost

- (b1)–(b3), the field, the setting and the guesser: **~1.5 days**.
- (b6) the one composer on both roads: **half a day**.
- (b4) B's classifier: **half a day on this side**, and only once B says the array can ride an
  existing request.

**~2.5 days for (b), half a day for (a).** (a) can go in the next install; (b) wants its own round
because it touches the Link's UI and its persisted state, and because the guesser is only worth
shipping once it has been run against his whole session's track names.

---

## Order I would take them

1. **(a)** with the next install — half a day, QUICK, and it closes "how much gain reduction is it
   doing?" on both roads and both racks.
2. **(b1)–(b3) and (b6)** as their own round, with the guesser's legs cut from his own track names.
3. **(b4)** when B confirms the names can ride a request that is already going.

**Nothing starts until he says.**
