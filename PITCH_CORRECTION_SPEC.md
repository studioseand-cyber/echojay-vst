# EchoJay Pitch — device #22

Real-time pitch correction with two characters: **Natural** (transparent, keeps the
performance, Melodyne-ish smoothness) and **Tuned** (immediate snap, the Auto-Tune effect).
Full control surface, and — the reason it belongs in EchoJay rather than being one more
tuner — **the key dials itself** from the Key Detector we already built.

---

## 0. What "as close to Antares" honestly means

Auto-Tune's algorithm is proprietary and not published. Nothing here reverse-engineers it,
and a session should not try — the result would be worse, not better, because it would be
guessing at internals instead of implementing methods that are documented and understood.

What we CAN match, and what this spec targets:

- **The control surface** — every knob a user reaches for on Auto-Tune Pro has an equivalent
  here, doing the same job, in the same units where units exist.
- **The behavioural envelope** — retune from instant to slow, correction that can be told to
  leave expressive pitch alone, vibrato that survives or gets replaced, formants that hold
  while pitch moves.
- **The quality bar** — PSOLA-family shifting with proper epoch alignment and formant
  preservation is what makes a corrector sound like a voice instead of a vocoder. That is the
  achievable target and it is a high one.

What we are NOT claiming: bit-identical behaviour, or that any particular preset "is"
Auto-Tune. Set that expectation now rather than at acceptance.

**Naming.** "Flex-Tune", "Throat Length" and "Auto-Tune" are Antares branding. Use our own
names for the same functions — the spec below does. Functional parity is fine; borrowing
their product vocabulary into a shipping competitor is not worth the argument.

---

## 1. Scope

**Monophonic** — one voice or one lead instrument per instance. This is what the Antares-class
correctors do, and it is what makes low-latency correction tractable. Polyphonic correction
(the thing Melodyne does that nothing else does) is a different and much harder machine; it is
explicitly out of scope and should not be smuggled in.

**Real-time only.** No offline note-graph editor. When Sean says the Natural mode should sound
"more like Melodyne", that is about the *smoothness and transparency of the result*, not about
building a note editor. Read it that way.

---

## 2. The DSP core

Four stages. Build them in this order; each is testable alone.

### 2.1 Pitch detection

Needs to be accurate to a few cents, fast to converge, and stable on breathy or noisy input.

Use a **YIN-family estimator** (difference function → cumulative mean normalised difference →
absolute threshold → parabolic interpolation of the chosen lag). It is published, well
understood, and its failure modes are known — which matters more than raw cleverness, because
the failures are what you will spend your time on.

- Analysis window: about 2–3 periods of the lowest expected pitch, driven by `voice_type`
  (a soprano window is much shorter than a bass one — this is the whole point of that control).
- Hop: 128 samples at 48k or thereabouts; the correction envelope is smoothed anyway.
- **Octave errors are the enemy.** YIN halves and doubles under vibrato and on breathy onsets.
  Guard with a short median over recent estimates plus a continuity bias toward the previous
  f0. Log how often the guard fires — if it fires constantly the window is wrong for the
  material, not the guard.
- Emit alongside f0: a **confidence** (aperiodicity) and a **voiced/unvoiced** flag. Unvoiced
  frames must pass through untouched. Correcting a consonant is exactly what makes cheap
  correctors sound like cheap correctors.

### 2.2 The decision — where should this note be?

Given f0, the reference pitch, and the active scale, choose a target:

1. Convert f0 to cents relative to `reference_hz`.
2. Find the nearest **enabled** scale degree (§5's `pitch_scale`), applying that degree's
   `bias_cents` if set.
3. Apply `flex`: below the flex threshold, correction is scaled down toward zero, so small
   expressive deviations survive. At `flex = 0` every deviation is corrected; at 100 only
   gross errors are.
4. Apply `humanize`: reduce correction strength on **sustained** notes while leaving onsets
   fully corrected. Sustained-vs-onset is decided from how long f0 has been stable, not from
   amplitude.
5. Apply `transpose`.

The output of this stage is a target f0 and a **desired correction in cents**, not yet applied.

### 2.3 The retune envelope — the character control

The correction is approached over time, not jumped to. `retune_speed_ms` is the time constant
of a one-pole toward the target, with two refinements that matter:

- **Note-change detection resets it.** On a genuine note change the envelope should start from
  the new note rather than gliding from the old one, or every interval turns into a portamento.
  Detect via a cents-jump threshold plus a re-onset.
- **`targeting_ignores_vibrato`**: when on, the *target selection* uses a slow-smoothed f0 so a
  wide vibrato doesn't cause the target to flip between adjacent scale degrees, while the
  *correction* still tracks the fast f0. Without this, wide vibrato on a semitone boundary
  chatters between two notes — an unmistakable artefact.

- **The envelope must HOLD across untracked gaps, not reset.** The detector does not produce
  an f0 for every frame: `tracking` gates out frames whose pitch is genuinely ambiguous, and
  at the `normal` default that is ~13% of otherwise-voiced frames on real material. Measured
  (`PITCH_P0_VALIDATION.md` §5.3) those gaps are **median 11 ms**, p95 ~59 ms, with a
  worst observed case of **142 ms**. Across such a gap the envelope must freeze its target and
  its current position and resume from there, so correction is continuous through a consonant
  or a breath rather than restarting after every one.

  **This is the same code path as note-change reset, and that is the trap.** §2.3's reset
  fires on "a cents-jump threshold plus a re-onset", and a gap resume looks exactly like a
  re-onset. If the resume is treated as a note change, the envelope re-glides from the wrong
  place on every consonant — audible as a scoop into the back half of every word, and easy to
  misread as a PSOLA artefact. **Distinguish them by gap duration**: a resume within roughly
  200 ms of the last tracked frame continues the existing note; longer than that, or with a
  target that has moved by more than the note-change threshold, is a genuine new note. The
  142 ms worst case sits under that line deliberately, and `tight` was chosen (0.80, not
  higher) to keep it there — see `PitchEngine::kTrackingConfidence`.

At `retune_speed_ms = 0` with `flex = 0` and a tight scale you get the hard-tuned effect. At
120 ms with flex and humanize up you get transparent correction. **This is where the two
characters actually live** — see §4.

### 2.4 The shift — PSOLA with formant preservation

**TD-PSOLA**, epoch-synchronous:

- Find pitch epochs (glottal closure instants) from the detected period — a peak-picking or
  normalised-cross-correlation approach inside each period window is adequate.
- Window two periods, Hann, centred on each epoch.
- Re-space the windowed grains at the target period and overlap-add.

PSOLA earns its place here because re-spacing grains **moves pitch without moving the spectral
envelope** — formants stay put for free, which is most of why it sounds like a voice. That is
also why the formant control is a deliberate *departure* rather than a correction:

- `formant_mode = preserve` — plain PSOLA; formants stay where they were.
- `formant_mode = shift` — estimate the spectral envelope (LPC, order ~ 2 + fs/1000, or
  cepstral liftering), flatten, shift pitch, re-apply the envelope warped by `formant_shift`.
  This is the "throat length" control by another name: negative shifts sound bigger/deeper,
  positive smaller/brighter.
- `formant_mode = off` — resample-style shifting, formants move with pitch. Chipmunk territory,
  and occasionally exactly what someone wants.

**Latency.** PSOLA needs roughly one analysis window of lookahead. Report it honestly via
`setLatencySamples` so the host compensates — a pitch corrector that silently misaligns a vocal
against the track is worse than one that is a few ms slower. `low_latency` trades window size
(and therefore low-note accuracy) for delay; it must change the reported latency accordingly.

---

## 3. Signal path order

```
input → [unvoiced/silence gate] → pitch detect → decision → retune envelope
      → PSOLA shift (+ formant warp) → vibrato generator → mix (dry/wet) → output
```

Vibrato is added **after** correction on purpose: correcting a note and then adding vibrato is
coherent, whereas adding vibrato before correction just gives the corrector something to fight.

---

## 4. The two characters

`correction_mode` is the headline control and the thing Sean asked for. It is **not** a separate
algorithm — it is a named point in the parameter space, and it moves the underlying knobs so the
user (and the model) can see what it did:

| | `natural` | `balanced` | `tuned` | `hard` |
|---|---|---|---|---|
| `retune_speed_ms` | 120 | 40 | 8 | 0 |
| `flex` | 55 | 25 | 0 | 0 |
| `humanize` | 60 | 30 | 0 | 0 |
| `targeting_ignores_vibrato` | on | on | off | off |
| `natural_vibrato` | 100 (kept) | 100 | 40 | 0 |
| `formant_mode` | preserve | preserve | preserve | preserve |

**Selecting a mode writes those values into the visible params.** It does not hide them. The
user turns any knob afterwards and the mode display goes to `custom`. This matters for
dialability: the model can say "natural" and get a coherent starting point, *or* dial the
individual controls, and both routes are legible in the same UI. A mode that secretly changes
behaviour without moving knobs is undialable by definition — the model can't see what it did.

---

## 5. The dialable contract

`ParamSchema`, descriptions written **for the model** — each says what the control does to the
sound, because "make the vocal sound obviously tuned" has to be translatable against this text
alone.

| id | unit | range | default | notes |
|---|---|---|---|---|
| `correction_mode` | | choices | `natural` | `natural`, `balanced`, `tuned`, `hard`, `custom` |
| `retune_speed_ms` | ms | 0..400 | 120 | how fast pitch is pulled to the target; 0 is the hard tuned effect, 100+ is transparent |
| `flex` | % | 0..100 | 55 | how much expressive drift is left alone before correction engages; high keeps slides and scoops |
| `humanize` | % | 0..100 | 60 | relaxes correction on sustained notes while keeping onsets tight, so long notes don't sound frozen |
| `key_source` | | choices | `auto` | `auto` (follow the detected key of the music), `manual` |
| `key_root` | | choices | `C` | `C`..`B`; ignored when `key_source = auto` |
| `scale` | | choices | `major` | `major`, `minor`, `harmonic_minor`, `dorian`, `mixolydian`, `major_pentatonic`, `minor_pentatonic`, `blues`, `chromatic`, `custom` |
| `reference_source` | | choices | `auto` | `auto` follows the detected tuning of the music, `manual` uses `reference_hz` |
| `reference_hz` | Hz | 380..500 | 440 | concert pitch reference |
| `transpose` | st | −12..12 | 0 | shifts the corrected result in semitones |
| `voice_type` | | choices | `alto_tenor` | `soprano`, `alto_tenor`, `low_male`, `instrument`, `bass` — sets the pitch search range; wrong choice causes octave errors |
| `tracking` | | choices | `normal` | `relaxed`, `normal`, `tight` — how strict the detector is before it calls a frame pitched; relaxed for breathy, tight for clean |
| `formant_mode` | | choices | `preserve` | `off`, `preserve`, `shift` |
| `formant_shift` | st | −12..12 | 0 | moves the vocal character independently of pitch; negative is bigger/deeper, positive is smaller/brighter |
| `natural_vibrato` | % | 0..200 | 100 | how much of the singer's own vibrato survives; 0 removes it, above 100 exaggerates it |
| `targeting_ignores_vibrato` | | on/off | on | stops wide vibrato flipping the target between neighbouring notes |
| `vib_depth_cents` | ¢ | 0..100 | 0 | depth of ADDED vibrato; 0 is off |
| `vib_rate_hz` | Hz | 0.1..10 | 5.5 | rate of added vibrato |
| `vib_shape` | | choices | `sine` | `sine`, `triangle`, `ramp` |
| `vib_onset_ms` | ms | 0..3000 | 300 | delay before added vibrato fades in, so it starts like a singer would |
| `vib_amp_amount` | % | 0..100 | 0 | how much the added vibrato also moves loudness |
| `vib_formant_amount` | % | 0..100 | 0 | how much it also moves formants — small amounts read as more human |
| `low_latency` | | on/off | off | shorter analysis window: less delay, less accurate on low notes; changes reported latency |
| `mix` | % | 0..100 | 100 | blend against dry; below 100 is parallel/doubled correction |
| `output_db` | dB | −24..24 | 0 | |

### 5.1 The scale as an object move

Per-degree control needs a structured move, the same shape as `eq_bands`:

```
pitch_scale: [ { semitone: 0..11, enabled: bool, bias_cents: -50..50 }, ... ]
```

**Merge semantics, keyed on `semitone`** — an omitted degree keeps its current state. This is
deliberately the opposite of `eq_bands`' replace semantics, and it is the right choice here
because "remove the 7th from the scale" is a single-degree edit, not a redefinition of the
scale. Whichever way it lands, the behaviour must be stated in the advertisement so the model
knows which it is getting; a move whose semantics the model has to guess is not dialable.

Setting `scale` to a named value rewrites all twelve degrees. Setting any degree afterwards
moves `scale` to `custom`. Same visible-state rule as `correction_mode`.

---

## 6. The key auto-map — why this device belongs in EchoJay

This is the part no competitor can do, and it uses machinery that already exists.

When `key_source = auto`:

- The device reads the same `[DETECTED KEY]` fact the AI feed uses — from a Link on the
  music/instrumental bus by preference (`placement == bus`), falling back to a channel reading,
  exactly as §9.2 of `KEY_DETECTOR_SPEC.md` already specifies.
- `key_root` and `scale` display the detected values, greyed, tagged with the source:
  `from "Music Bus"`. The user sees where the key came from — attribution is not decoration,
  it is what makes a wrong reading diagnosable.
- `reference_source = auto` does the same with the detected tuning, so a track recorded at
  441.3 Hz gets corrected to 441.3 Hz rather than being dragged to 440 and sounding subtly
  wrong against everything else. **This is the detail that will make people notice the plugin
  is good**, and it falls out of work already done.

**The confidence gate.** Below the §4 confidence threshold (~0.5), do NOT auto-apply a key.
Fall back to **`chromatic`** — correct to the nearest semitone with no scale. Chromatic is the
musically safe failure: it still tunes, and it cannot force a note that is wrong for the song.
Falling back to the last known key would be worse, because a stale key is applied with
confidence. Show the fallback in the UI; never fail silently.

**The precondition.** If `key_source = auto` and no key is known at all, this is exactly the
case `KEY_PRECONDITION_SPEC.md` handles — the two-tier add-or-ask that offers to put a Key
Detector on the mix bus. Reuse it; do not write a second prompt. A chain request that includes
this device should trigger the same precondition a chain with autotune already does.

**Live key changes.** If the detected key changes mid-session (a modulation, or a new song),
cross-fade the scale over a few hundred ms rather than switching on a sample. A hard scale
switch under a sustained note is audible.

---

## 7. Visualization

The signature view is a **pitch ribbon**, and it is the most legible visualization in the whole
suite because the thing being shown is the thing being corrected:

- Horizontal: time, scrolling, ~4 seconds.
- Vertical: pitch, with **scale degrees as horizontal lines** — enabled degrees bright,
  disabled dim. The key is visible at a glance.
- Two traces: **detected pitch** (dim) and **corrected pitch** (bright). The gap between them
  *is* the correction, so retune speed and flex become directly visible: fast retune shows the
  traces snapping together, high flex shows them running parallel and apart.
- Unvoiced frames drawn as gaps, not as zero — a line dropping to the floor on every consonant
  looks like a bug.
- Reuse the dwell-glow easing from the EQ (`1 - exp(-dt/tau)`, τ≈120 ms) for the trace so it
  reads smooth rather than steppy. That work is done; don't rewrite it.

A small **note wheel** (the Key Detector's, reused) shows the current target degree.

---

## 8. Correctness rules that are cheap now and expensive later

- **Unvoiced and silent frames pass through bit-identical.** Test it with a null: correct a
  spoken-word file with the scale set to chromatic and retune 0, and the consonants must null.
- **Bypass is click-free** and reports the same latency, so bypassing doesn't shift timing.
- **Latency is reported accurately** and changes when `low_latency` does.
- **Sample-rate independence** — every time constant in ms, every window in ms, converted at
  `prepareToPlay`. Test at 44.1, 48 and 96k.
- **No allocation, no locks, no logging on the audio thread.** PSOLA grain buffers are
  pre-allocated at the maximum window `voice_type` can ask for.
- **The editor-paint regression harness must construct and paint this editor**, like every
  other device — a viz that crashes on paint should fail a test, not a DAW.

---

## 9. Build phases

**P0 — detection only.** YIN + confidence + voiced flag, no shifting. Ship a debug readout of
detected pitch in cents. Prove it on real vocals across all five `voice_type` settings and log
octave-error rate. *Do not proceed until this is solid — every later stage inherits its
errors, and a correction artefact caused by a detection error looks like a PSOLA bug and will
be debugged as one.*

**P1 — PSOLA shift**, fixed target (e.g. "everything to A440"), formants preserved. Prove
quality on sustained notes before any musical logic exists.

**P2 — decision + retune envelope**: scale, flex, humanize, note-change reset, ignore-vibrato.
This is where it becomes a pitch corrector.

**P3 — the dialable contract**: full ParamSchema, `correction_mode` writing visible params,
`pitch_scale` object move, registrar, advertisement.

**P4 — key auto-map**: `[DETECTED KEY]` wiring, tuning follow, confidence gate to chromatic,
precondition reuse.

**P5 — vibrato, formant shift, low-latency mode, the pitch ribbon.**

P0–P2 are one session's work and should be one worktree — they are a single tightly-coupled
machine and splitting them across sessions would mean merging half-working DSP. P3 onward can
fan out.

---

## 10. Acceptance

1. A flat vocal on a track in F# minor, `key_source = auto`, no key told to the plugin: it
   corrects to F# minor, and the UI names the Music Bus as the source.
2. `correction_mode = natural` on a good take is **hard to hear** — pitch is tidier, the
   performance is intact, no artefacts on consonants.
3. `correction_mode = hard` on the same take is unmistakably the effect.
4. Selecting a mode visibly moves the individual knobs.
5. "make the vocal sound obviously tuned" → the model reaches `hard` (or retune 0 + flex 0),
   applied, and the applied line names what changed.
6. "tune this but keep it natural" → `natural`, not `hard`. If the model can't tell these
   apart, the schema descriptions are wrong — that is a text fix, and it is the finding, not
   a nuisance.
7. Detune the track's reference to 441.3: with `reference_source = auto` the corrected vocal
   sits with the music, not against it.
8. Kill the Link: the device falls to chromatic, says so, and keeps working.
