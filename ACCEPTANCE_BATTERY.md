# EchoJay — Acceptance Battery

The question this answers is not "does it work". It is: **is the whole suite dialable by
prompt, or is only the EQ dialable and the other twenty devices merely present?**

Everything shipped so far has been proved on the EQ. The EQ is the device with the richest
move schema, its own object move, and the most attention paid to it. That makes it the least
representative device in the build. This battery deliberately exercises the ones nobody has
prompted at yet.

Run it against the installed dev build (`8cea0d1` or later, DEV transport on, Logic fully
relaunched so `dev.json` is read).

---

## How to read a test

Each test gives a **prompt**, what a **pass** looks like, and — the part that matters — a
**failure signature** naming the layer to look in. Without that, a failed test starts a guess,
and this project has already paid for guessing twice (the stale `ej-time` binary, and my
confidence-threshold detour). A failure should route, not open an investigation.

Record every result. A test that "seemed fine" is not a result.

---

## Test 0 — ground truth, before any prompt

Do not trust this document's param names over the build's. The advertisement is what the model
actually reads, so it is the only authority on what exists.

In a chat turn on the installed build, ask:

> list every built-in device you can dial, and for each one every parameter id, its range, and
> its choices if it has any

Save the reply. **This is the reference for the whole battery.** Two things to check in it:

1. **All devices are present** — 21 plus the Key Detector, across EQ, Dynamics, Utility,
   Stereo, Modulation, Harmonic, Time. A missing category means the registrar for that cluster
   was dropped by the static linker again → `echojay_force_load_shared_code()`.
2. **Every id below appears in it.** Where this document and the advertisement disagree,
   the advertisement wins and this document is wrong — fix the doc, do not "fix" the build.

Failure signature: the advertisement is absent entirely → `echojayBuiltinsBlock()` is not
reaching the feed path this turn took. It was attached to only one of the two builders once
before (`8e963a7`); the regression is cheap to reintroduce and invisible to users without
third-party plugins. Check both `buildPluginInjection` and `buildChainInjection`.

---

## Tier A — one param move per category

Purpose: prove `settings_structured.params` lands on a device from every category, so a
category-wide schema or apply break can't hide. One device from each; add the device to a chain
first, then prompt.

| # | Device | Prompt | Pass |
|---|---|---|---|
| A1 | Compressor | "set the compressor to a 4:1 ratio with a 30ms attack" | Both knobs move; the reply names both changes |
| A2 | Gain | "drop the gain 3dB and pan it slightly left" | `level_db` −3, `pan` negative |
| A3 | Stereo Width | "narrow the low end below 120Hz to mono" | `bass_mono_hz` ≈ 120 |
| A4 | Chorus | "slower, wider chorus" | `rate_hz` down, `spread`/`depth` up |
| A5 | Saturation | "a bit more drive but keep it clean" | `drive_db` up modestly, not slammed |
| A6 | Delay | "eighth-note delay, 25% feedback, quiet in the mix" | `sync` on, `sync_division` = 1/8, `feedback` 25, `mix` low |
| A7 | Surgical EQ | "cut 3dB at 300Hz with a narrow Q" | A band appears at 300Hz, −3dB, Q high |

Pass for the tier: **7/7 apply, and the applied-summary line names each change.** The applied
line is the contract — a change that happens silently is a change the user cannot trust.

Failure signature — one device fails, rest pass → that device's `applyStructured` or its schema
(check id spelling against Test 0). A whole category fails → the category's registrar or a
shared base. Every device fails → the move never arrived; look at the backend's move parsing
before touching any plugin code.

---

## Tier B — modes and choices, the depth pass

This is the tier most likely to fail, because it is the newest and because choice params dial
differently from continuous ones: the model must pick a **name**, and the apply path must map
that name to an index. An off-by-one here is silent — you get a plausible wrong mode.

| # | Prompt | Pass |
|---|---|---|
| B1 | "make the compressor glue the mix together" | Compressor `mode` = **glue**, not clean, and probably a slower attack |
| B2 | "punchy compression for drums" | `mode` = **punch** |
| B3 | "the opto one, for the vocal" | `mode` = **smooth** |
| B4 | "use tape saturation instead of tube" | Saturation `type` = **tape** |
| B5 | "warm cassette tape character" | Tape `mode` = **cassette** |
| B6 | "put it in a plate reverb" | Reverb `algorithm` = **plate** |
| B7 | "compress off the peaks, not the loudness" | Compressor `detector` = **peak** |
| B8 | "vintage phaser" | Phaser `mode` = **vintage** |

**Verify by looking at the knob, not the reply.** A reply saying "set to glue" while the knob
sits on clean is exactly the failure this tier exists to catch, and it reads as a pass if you
only read the prose.

Failure signature — the right *name* is chosen but the wrong mode lands → index mapping in the
apply path (an off-by-one, or a choices list ordered differently from the enum). The wrong name
is chosen → the choice **descriptions** are not doing their job; that is a schema-text fix, not
a code fix, and it is the more valuable finding of the two.

---

## Tier C — object moves

| # | Prompt | Pass |
|---|---|---|
| C1 | "give me a vocal EQ: high-pass at 90, a 2dB lift at 3k, and take 2dB out at 400" | Three bands, `eq_bands` **replace** semantics — no leftovers from a previous state |
| C2 | (after C1) "now also take out 250Hz" | Correct semantics observed and stated: does it replace the set or add to it? Whichever the spec says, it must be consistent |
| C3 | Multiband: "compress the low band harder, leave the top alone" | `comp_bands` **merge** — the untouched band keeps its settings |

C3 is the one to watch. Merge semantics mean an omitted band must be *preserved*, not reset to
default. A move that quietly defaults the bands it didn't mention is the same class of bug as
replace-vs-merge confusion and destroys user state.

---

## Tier D — the interpretive prompts

No device named, no parameter named. This is what a user actually types, and it is the only
tier that tests whether the schema descriptions are written for the model rather than for a
manual.

- **D1** — "tame the harsh resonances" → expect surgical narrow cuts in the 2–5k region, not a
  broad shelf, and not a de-esser reach unless the material is vocal.
- **D2** — "make this vocal sit in the mix" → expect a multi-device move: some subtractive EQ,
  compression, probably a touch of the de-esser. Several devices, one turn.
- **D3** — "it sounds muddy" → expect action around 200–400Hz.
- **D4** — "too bright and thin" → expect the *opposite* direction to D3, which catches a model
  that has learned one gesture and applies it to everything.

Pass is not "the perfect move". Pass is **plausible, in the right frequency region, in the
right direction, and applied**. Judge direction and region; do not judge taste.

Failure signature — D1–D4 all produce the same shape of move → the descriptions are not
discriminating and the model is pattern-matching on "EQ request". That is fixed in schema text.

---

## Tier E — cross-channel key (the one that justifies the architecture)

Setup: a Link on the music/instrumental bus, EchoJay on a **vocal** channel, music playing long
enough for a confident read.

- **E1** — key reads on the Link and appears as `[DETECTED KEY]` with `source:` naming the bus
  and a `placement` of bus.
- **E2** — "build a melodic rapper's vocal chain" → the model receives the key **from the music
  bus** and acts on it (tunes a delay, avoids notching the root, places a bell off the third).
  It must not read the vocal.
- **E3** — remove the Link → the block disappears **entirely**. It must not fall back to
  reading the vocal's key. Falling back is worse than no reading, because a wrong key is acted
  on with confidence.
- **E4** — EchoJay on the mix bus with no Link at all → it detects its own channel (the
  self-detection case, fixed after you found it). The disqualifier is "not the music", judged by
  declared role — not "is my own channel".
- **E5** — RESET, then a different song → a new key within a few seconds. This is the exact bug
  you caught (RESET zeroed the counter but left `lastLiveAt_`/`armedAt_`/`lastContinuousAt_`
  stale). Re-run it deliberately; regressions return to the place they were fixed.
- **E6** — the source dropdown: aim the analyser at another channel and confirm the reading
  follows the selection.

---

## Tier F — regressions that already shipped once

Each of these was live in a build at some point. They get a permanent test because the reason
each shipped was structural, not careless.

- **F1** — a reply that echoes `[ECHOJAY BUILTIN MOVES]` must not blank the message. (Shipped
  because `test-reply-scrub` sat *outside* the gate.) Confirm it is inside the gate now.
- **F2** — `[DETECTED KEY]` must never appear as visible text to the user — header stripped
  *and* payload stripped, the `SCRUB_TRAILING_PAYLOAD` case.
- **F3** — the assistant must not ask which channel it is on, and must not volunteer capture.
  Test on a **main-plugin turn**, not a link turn — the guidance lived inside
  `if (targetLinkUid.isNotEmpty())` and main-plugin turns never saw it.
- **F4** — say "yes" to a proposed chain → it builds. It must not answer a confirmation with a
  question.
- **F5** — a turn carrying `[THIS CHANNEL]` must not be misread as a chain request by
  `CHAIN_REQUEST_RE`. Verified by reading, but worth one live turn.

---

## Failure routing

| What you see | Where it is |
|---|---|
| No device responds to any move | Backend move parsing / the feed, not the plugin |
| One category dead | Registrar dropped — `echojay_force_load_shared_code()` |
| One device dead | That device's schema or apply |
| Right name, wrong mode lands | Choice index mapping |
| Wrong name chosen | Schema description text |
| Reply says it moved, knob didn't | Apply path silently failing — the worst class; it looks like a pass |
| Key from the vocal, not the music | Link frame / source preference (`placement == bus` first) |
| Prose vanishes from a reply | Scrub — check for an unterminated marker |

---

## Recording

For each test: id, pass/fail, and on failure the **exact reply text** plus what the knob
actually read. The reply alone is not evidence — the pairing is. Send me the failures with both
halves and I can tell you which layer without another round trip.
