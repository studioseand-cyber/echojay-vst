# Item 0a — GR and the analyser, from another track's Link

**Design, 14 Oct 2026. Nothing here is built.** Written while gate10 runs.

**RULED, 14 Oct:** Sean agreed every recommendation in §6 (Q1-Q4) — one shm object per Link, GR plus the
calibration block first, no publishing while a Link is inactive, and the GR max-hold resetting with the
existing tally. **With one change to Q2:** the analyser follows GR and calibration **in the same push,
before beta** — not a later round. So the region is built to its full shape from the start and the analyser
is the second half of one piece of work, not a sequel.

**Build order:** this comes after item 0 + 0b and Auto mode.

---

## 1. Why a new region, and not another field

`LinkMeterFrame` is **full, and its size is the mapping's stride**:

```cpp
static_assert(sizeof(LinkMeterFrame) == 128, "LinkMeterFrame must be 128 bytes");
static constexpr size_t kRegSize = sizeof(RegistryHeader)
                                 + kRegMaxSlots * sizeof(RegistrySlot)
                                 + kRegMaxSlots * sizeof(LinkMeterFrame);
```

Growing the frame moves **every frame after slot 0** under any instance that has not been rebuilt, and mixed
pairs are normal on Sean's machine. The pad has been spent twice already: 21t-d took the last eight bytes
(`shortTermMax`, `heardSeconds`), and 21t-f had to quantise SHORT90 into **one byte** of the key group's pad
because, in the header's own words, *"the only honest four bytes do not exist."* `RegistrySlot` is in the
same state — two bytes of `_pad` left.

GR per slot plus an analyser needs **hundreds** of bytes. So: a new region.

### Two ways, and which I recommend

**(A) A separate shm object per Link instance — recommended.**
`/ej_dm_<uid>` (uid is 12 chars, so the name fits macOS's 31-char limit), created by the Link, mapped by a
reader only when it wants deep meters for that uid.

- the registry mapping **does not change size**, so nothing an installed binary holds moves. This is the
  whole argument.
- **existence is the capability**, which keeps the convention `dialCapable` set: *"keep the fallback the SAFE
  answer, not the convenient one."* No object → the reader says it cannot measure that Link, exactly as it
  does today. No version number is consulted.
- it has **its own header and its own version**, so it can grow later without this conversation again.
- cost: one more shm object per Link, one more thing to unlink on teardown (the registry's existing reaper
  pattern — `publisherPid` + `kill(pid, 0)` — extends to it).

**(B) Append a third array after the frames in the same mapping.**
Offsets of everything existing stay put, so old readers are fine — but the **file's size** changes, and
whoever claims first creates it. A new Link would have to grow a file that old binaries have already mapped,
and a new reader must handle a short file. Workable, cheaper, and it puts a resize race in the middle of the
one mapping every instance depends on. **Not recommended.**

## 2. What the region carries

```
┌─ DeepMeterHeader (64 B) ────────────────────────────────────────┐
│ magic, version, byteSize, publishRate Hz, publisherPid          │
│ seq (seqlock, odd = write in progress), fieldsMask              │
├─ slots: 15 × DeepSlotMeter (kMaxChainSlots = 15, both sides) ───┤
│ identity[16]  the slot's identity, NOT its index                │
│ grDb          gain reduction, dB, 0 = none                      │
│ grMaxDb       max-hold since the tally reset                    │
│ inDb, outDb   the slot's own in/out, dBFS                       │
│ flags         bypassed | grKnown | measuring                    │
├─ analyser ──────────────────────────────────────────────────────┤
│ bands[64]     dBFS per display band (matches spectrumBands[64])  │
│ macroBand[6]  sub..air, dB rel to band mean (as the frame has)   │
│ bandAgeMs     how old the picture is                             │
├─ calibration / landing (EJCalibLoop) ───────────────────────────┤
│ loopState     Idle|Listening|Waiting|Adjusted|Clamped            │
│ purpose       buildHold|askRung                                  │
│ actuator      Drive|Threshold|Input                              │
│ currentGainDb, targetDb, toleranceDb, movedDb                    │
│ heardSeconds, makeupCapDb                                        │
│ stage[32]     the loop's own words for where it is               │
└─────────────────────────────────────────────────────────────────┘
```

Roughly 15×32 + 64×4 + … ≈ 900 B; **round the region to 4 KB** (one page) and put the spare in an explicit
pad, so the next field is not another SHORT90.

**Three things that are not negotiable, each from a fault already in this codebase:**

1. **GR is keyed to the slot's identity, never its index.** The undo history found the same thing and
   settled it the same way (*"a slot by identity `uid|name`, index as the fallback"*): a reorder or a
   substitution between publish and read would otherwise attribute one plugin's GR to another.
2. **`grKnown` is a flag, and absent means absent.** A compressor whose GR cannot be read must not publish
   0.0 dB — `0.0` is a real measurement. This is the same trap the frame's `fieldsMask` comment describes:
   *"an ungated dB field reading 0.0f would render as a real measurement."*
3. **seqlock, same convention as the frame** (odd = write in progress; staleness detected by `seq` not
   advancing, no cross-process clocks).

## 3. Update rate

- **GR and the analyser at ~10 Hz**, the cadence the Link already publishes meters at while **Active**.
- **calibration/landing on change plus every publish**, since the agent reads it to answer "did it land?".
- the agent reads **on demand**: `check` takes one fresh snapshot and waits for `seq` to advance once, which
  bounds the read instead of sampling whatever instant it arrived in. (The 8 Sep rule: *wait with a bound, or
  say it is provisional.*)
- nothing is published when the Link is not Active, and `audioStale` already says when the host has stopped
  calling `processBlock` — **the publisher owns that truth**, and a GR of 0 on a frozen engine must read as
  absent, not as "no compression".

## 4. What the agent can then do

Today every measurement on another track's Link is refused, honestly and deliberately
(`ExecutorRead::notFromALink`, `EJAgentExecutorRead.cpp:224`):

> *"gain reduction cannot be read from a Link yet - only from the rack this EchoJay window has open. This
> will not work on a retry."*

With the region:

- `ExecutorRead::measuringALink` stops being a **blanket** refusal and becomes a **capability question**: is
  there a deep-meter region for this uid, and is it fresh? The refusal text stays, word for word, for a Link
  that has none — an older Link, or one that is not Active. That sentence took eighteen rounds of Sean's
  loop to get right and it is still the right answer when the answer is no.
- `check(gain_reduction, channel: <uid>)` answers from `grDb`/`grMaxDb` with the slot named by identity.
- `look(analysis, channel: <uid>)` answers from `bands[64]`/`macroBand[6]` with an age.
- `check(level_landed, channel: <uid>)` answers from the calibration block — which is Sean's item 4 made
  **readable from the other side**: the rig currently proves a Link build's level match by measuring audio
  in the harness, and the agent cannot see it at all.
- the honesty rows the rig holds (H3) stay honest: with the region absent it still says plainly that it
  cannot measure a Link, and never "slot not on rack".

## 5. What B must change when it lands

1. **The tool schema**: `check` and `look` accept `channel` for `gain_reduction`, `analysis` and the
   landing state. Today the contract has them as own-rack only.
2. **The model's instructions**: stop telling it that a measurement on another track is impossible — it
   becomes *"possible when that Link publishes deep meters; ask look(rack) first if unsure."* The plugin
   still answers `not_available_on_link` per track, so the model must read the capability from the **answer**
   rather than assume it from the build.
3. **A new refusal code is not needed** — `not_available_on_link` keeps its meaning (this Link, now), with
   `retryable:false` as it is.
4. **The ladder for "make it punchier" on another track** can then measure instead of guessing, which is
   B's row to rewrite in `AGENT_CAPABILITIES.md` (G-series).
5. Nothing changes on the server for the region itself: it is plugin-to-plugin, inside the user's machine.

## 6. Questions for Sean

1. **One object per Link, or one shared region appended to the registry?**
   *Recommendation: one object per Link (A).* It cannot move a byte of what is installed.
2. **Does the analyser belong in the first version, or GR alone?**
   *Recommendation: GR plus the calibration block first, analyser second.* GR and "did the level land" are
   what the agent keeps being asked for; 64 bands are the bulk of the region and the least asked for.
   **RULED: both, in the same push, before beta.** GR and calibration land first inside that push so they
   can be driven on the rig while the analyser is written, but neither ships without the other. The region
   is therefore laid out at full size from the first commit (§2), and the analyser's `bandAgeMs` gate is
   what makes a half-filled region readable in the meantime: absent is absent.
3. **Should a Link publish deep meters while it is NOT Active?**
   *Recommendation: no.* Active is the user's own switch for "this Link may meter", and quietly metering an
   inactive Link would spend CPU the user asked not to spend.
4. **Does GR max-hold reset with the existing tally, or on its own?**
   *Recommendation: with the existing tally*, so one reset means one thing everywhere.
