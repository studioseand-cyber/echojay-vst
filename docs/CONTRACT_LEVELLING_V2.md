# Levelling v2: the Level slot goes, and each rack levels itself

**Status:** design, 10 October 2026. Sean's ruling after test 1 of `ship_2026-10-09a`. Supersedes the slot-based
half of `CONTRACT_LEVEL_PARAMS.md` (B's file); the `option` vocabulary and the target rules carry over unchanged.
**Nothing here is built yet.** Where a control already exists it is named and marked **EXISTS**.

---

## 0. Why, from test 1

Sean's vocal build on Link `Aitch_4_01`, edited from the V2 under a lease: the Level stayed at **0.0 dB**, its card
read `IN/OUT -23.3 LUFS-S`, `target -9.0 LUFS (match)` and `gain_db 0, target_lufs -9, loudness_option match`, and
the slot sat **5th of 6, before Vocal Reverb**. Three causes, all confirmed from the 09:00-09:15 logs:

1. **One loop, bound to the wrong rack.** `LoudnessLoop loudnessLoop_ { chainHost }` (PluginProcessor.h:1453) is a
   single instance holding a reference to the **V2's own** ChainHost. There is no loop on `borrowHost_`. The build
   went to the Link (`EJDialSummary(Link): channel build ... 4 slot(s)`), so the loop could not reach it. The two
   calls sit two lines apart in one function: `finishChainBubbleWhenDialSettled` reads
   `processorRef.getChainHost()` at :23751 and calls `armLoudnessLoopIfTargeted()` at :23775.
2. **It never armed.** **Zero** `EJLoudness` lines in all five logs - not "not armed: no target", nothing. The
   leased build path never reached the arm, so nothing ever drove the slot the server had dialled.
3. **A Level before a reverb cannot level a chain**, whatever drives it. That placement was the SERVER's (our
   `ensureLevelSlot` only runs when the loop arms, which it did not).

Cause 3 is unfixable by moving a slot around; cause 1 is unfixable while one loop serves two racks. Hence:

---

## 1. The decision

**The `EchoJay Level` slot is dropped for levelling.** The loudness loop stays and drives **gains that already
exist**, at the only two places that can be right:

| aim | the gain it drives | control (EXISTS) |
|---|---|---|
| **match** (channel / bus) | the rack's own **post-rack OUT gain** - after every slot, reverb included | a Link: `LinkProcessor::setGainDb`. The V2's own rack: `EchoJayProcessor::setBusGainDb` |
| **target** (mix bus / master) | the **final limiter slot's IN gain** (slot pre-trim), ceiling held at -0.1 dBTP | `ChainHost::setSlotPreTrimDb(limiterSlot, db)` |

**Insertion:** a **target** build whose chain does not end in a limiter gets an `EchoJay Limiter` inserted last at
-0.1 dBTP, because a target drives level INTO a ceiling and there must be one. A **match** build inserts
**nothing** - it is not pushing into anything. (This also retires last night's open question about substituting on
a chain with no limiter: a match build needs no limiter, and a target build gets a real one rather than having one
swapped in for a plugin the user asked for.)

---

## 2. WHERE EACH LOOP RUNS - the part that makes levelling work in place

**Every Link runs its OWN loudness loop on its OWN rack.** Sean, 10 Oct. Not one loop reaching across a lease;
one loop per rack, living in the process that owns the audio.

- `LinkProcessor` gains a `LoudnessLoop` bound to **its** `chainHost`, driving **its** `gainDb_` (match) or its
  final limiter's pre-trim (target).
- `EchoJayProcessor` keeps its loop for **its own** rack (a V2 on the mix bus levels itself the same way).
- **No loop ever drives a rack it does not own.** `borrowHost_` gets no loop at all, and the whole class of
  cause 1 goes with it.

**The V2's part is to TRIGGER and to SHOW, over the remote channel** (`CONTRACT_LINK_COMMANDS.md`):

| direction | what travels |
|---|---|
| V2 -> Link | the trigger and the verbs: `level_arm`, `level_listen`, `level_check`, `level_go`, `level_leave`, `level_undo`, and `set_level {option, target_lufs?}` (the agent's tool, same stage) |
| Link -> V2 | the RESULT, in the meter frame the V2 already polls: aim, option, target, landed gain, loop state, and the aim words |

The V2 renders that result; it does not compute it. That is the same rule as the meters (§2 of the remote-control
plan): the Link is the only process that sees the audio, so it is the only one that may measure.

**Consequence, stated because it is a real one:** the match gain IS the user's own fader
(`setBusGainDb` is driven by the bus fader drag at PluginEditor.cpp:10975; the Link's `gainDb_` is the mixer
strip's GAIN). The loop and the user therefore share one control, deliberately - Sean's ruling is to drive gains
that already exist, not to add a hidden stage. The contention is handled by machinery that already shipped in
08c: a loop write stamps the chain's value revision, so a later user fader move makes the landing **stale**, and
the loop says so once and offers to re-land rather than fighting the user for the control.

---

## 3. The levelling record - at RACK level, not on a slot

One record per rack, persisted with the project and with a saved chain:

```
option       "match" | "commercial" | "pushed" | "dynamic"
target_lufs  number, integrated LUFS - absent in match mode
landed_db    the gain the loop set, on whichever stage §1 names
stage        "rack_out" | "limiter_in"
aim_words    "matched to input" | "-8.0 LUFS, commercial"
```

**Downgrade-safe by construction** (Sean's rule (b), 10 Oct): these are **new, additive** state fields. Nothing
the old build reads is removed or renamed. `ship_2026-10-09a` opening a new-build project finds no `EchoJay Level`
slot and an unknown rack-level record it ignores - and **the level is unchanged**, because the gain lives on the
rack OUT fader / limiter IN trim, both of which the old build has and restores. A leg asserts exactly that.

**Meters:** the existing chain IN/OUT tallies, and the loop-owned input window (`chainInLoopTally_`, 08c F2) so a
match compares IN and OUT over the same span without clearing the song's integrated reading.

**UI:** the result shows on the **OUT knob** (or the limiter's IN) with the words. **"target" never appears in
match mode** - the 08c card read `target -9.0 LUFS (match)`, which is two aims in one line and is the display
half of the same confusion.

---

## 4. Migration - no audible change

An existing `EchoJay Level` slot in a project or a saved chain:

1. read its `gain_db`;
2. add it to the rack's OUT gain (match) or to the final limiter's IN trim (target);
3. remove the slot;
4. carry `option` / `target_lufs` into the rack record;
5. log the before/after chain output so the "no audible change" claim is evidence, not an assertion.

The Level is a clean gain stage, so moving its dB post-rack is level-identical **except** where it sat before a
non-linear slot. That case is real (Sean's Level sat before Vocal Reverb) and it is the point: the sound changes
because the old placement was wrong, and the migration log must say so rather than claim nothing moved.

---

## 5. What B must change (`CONTRACT_LEVEL_PARAMS.md`)

1. **Stop emitting an `EchoJay Level` slot** in chain blocks. The plugin owns the gain stage and there is no slot
   to carry params on.
2. **Send the levelling record on the BLOCK, not on a slot**: `level: {option, target_lufs?}` beside `chain`.
3. **Keep** the option vocabulary and the target rules, with Sean's ruling 3: `match | commercial | pushed |
   dynamic`, commercial 10 dB and pushed 12 dB GR caps - so **emit `"commercial"`** rather than collapsing it to
   `"pushed"`, or the 10 dB cap can never be reached.
4. **A target build should end in a limiter**; if the block does not, the plugin inserts one at -0.1 dBTP and
   says so. A match build must not carry a limiter just for levelling.
5. `set_level` stays as B's contract has it; it resolves to the same two stages.

Until B changes, the plugin accepts today's shape: a Level slot in a block is **migrated on arrival** (§4).

---

## 6. Legs

| leg | asserts |
|---|---|
| match, chain ending in REVERB | out = in within 1 LU with the gain on the rack OUT, post the reverb - the case a Level slot could not do |
| target WITH a final limiter | lands the target, ceiling -0.1, gain on the limiter's IN trim |
| target WITHOUT a limiter | an EchoJay Limiter is inserted last at -0.1 and the target lands |
| match inserts nothing | no limiter, no Level, no slot of any kind added |
| migration of an old Level slot | gain moves to the right stage, slot removed, chain output unchanged (and the before/after is logged) |
| persistence | the record survives a reopen, and a saved chain carries it |
| **downgrade** | state written by the new build loads in the OLD schema: no Level slot, unknown record ignored, level preserved |
| per-Link loop | a Link levels its own rack with the V2 closed, and with the V2 open under a lease - neither needs the other |
| remote trigger | the V2's trigger reaches the Link and the result comes back in the meter frame; the V2 computes nothing |
| contention | a user fader move after a landing marks it stale and offers a re-land, rather than being overwritten |
