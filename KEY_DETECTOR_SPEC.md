
---

# 9. CROSS-CHANNEL — the key must come from the MUSIC, not the vocal

The problem: "build a melodic rapper's vocal chain" puts the chain on the **vocal**, but the
key lives in the **music**. A key detector that only analyses its own chain's signal reads
the vocal — a monophonic, sliding, often pitch-corrected source — which is the worst possible
input for key detection and, worse, is not the thing whose key matters.

**This needs no new system. EchoJay Link already is the cross-channel mechanism.**

## 9.1 The existing pattern (do not reinvent it)

- A **Link** instance sits on another channel/bus and measures it, publishing a
  `LinkMeterFrame` over shared memory (`LinkShm.h`): LUFS, RMS, peak, crest, correlation,
  width, `bandRel[6]`.
- The main plugin reads every registered Link's frame (`readLinkMeterFrame`) and already
  assembles them into an AI-feed block — `[LINK LEVELS — internal context ...]`
  (`PluginEditor.cpp` ~16664) — with each channel's display name, `uid` and **`placement`**
  (1 = bus, 2 = channel, 0 = unset).

Key is exactly the same shape of fact as loudness: a per-channel measurement, made where the
audio is, read where the decision is.

## 9.2 The design

**Key detection runs in TWO places, sharing one engine (`EedKeyEngine`):**

1. **In Link (the important one).** Link gains key detection over its existing analysis path
   and publishes the result in its frame. So a Link on the instrumental/mix bus knows the key,
   and the main plugin on the vocal can read it. This is what makes "build a vocal chain in
   the track's key" work at all.
2. **As the chain device** (`EchoJay Key Detector`, §1-§8). Still worth having: analyses
   whatever flows through the chain it is in, for when that IS the music, and it is where the
   full UI (note wheel, alternates, ANALYSE) lives.

**Frame additions** (append to `LinkMeterFrame`, never reorder — it is a shared-memory ABI;
bump the layout/version guard the header already uses):

```
int16_t keyRoot        = -1;      // 0..11 pitch class, -1 = none
uint8_t keyIsMinor     = 0;
float   keyConfidence  = 0.0f;    // 0..1
float   keyTuningHz    = 0.0f;    // detected reference pitch, 0 = unknown
uint32_t keyAgeMs      = 0;       // how stale the reading is
```

**Feed block.** Extend the `[DETECTED KEY]` block (§4) to name its source, and prefer a bus
reading over a channel reading when several exist:

```
[DETECTED KEY — measured by EchoJay on another channel; the KEY OF THE MUSIC]:
key: F# minor   confidence: 0.86   detected_tuning: 441.3 Hz
root_hz: 92.50 (F#2)   alternates: A major (0.72)
source: "Music Bus" (bus, uid m3k2)   age: 4 s
```

Rules:
- Prefer `placement == bus` sources; a bus reading is the mix, a channel reading is one part.
- If several Links report different keys, list the bus one and note the disagreement rather
  than silently picking — disagreement is itself information (a modulation, or a bad read).
- Carry `age`: a reading from 3 minutes ago on a different section is not current.
- The confidence rule from §4 still governs: below ~0.5, treat as unknown.

## 9.3 Why this ordering matters for the build

Build the **engine first**, then wire it into Link and the chain device — the engine is the
hard part and it is shared. Do not fork two implementations.

If Link work is being done in parallel by another session, the frame addition is the one
shared line: coordinate it, keep it append-only, and let the key engine land independently.

## 9.4 Acceptance, revised

With a Link on the instrumental bus and an EchoJay chain being built on the vocal:
"build a melodic rapper's vocal chain" → the model receives `[DETECTED KEY]` sourced from the
**music bus**, not the vocal, and can act on it (tune a delay, avoid notching the root, place
a bell off the third). Removing the Link removes the block entirely rather than falling back
to reading the vocal.
