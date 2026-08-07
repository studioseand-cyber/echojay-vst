# EchoJay Key Detector — device #21, and the first READER

The whole built-in suite so far is **writers**: the AI sets params, the device processes
audio. The Key Detector is the first **reader** — its value is not that the model dials it,
but that it tells the model something the model cannot otherwise know: what key the music
is in. That closes a loop the suite has been missing.

It is the same trick as the EQ's `tame_resonances` action, generalised: the AI names an
*intent* in musical terms and the plugin supplies the *numbers*.

---

## 1. What it is

`EchoJay Key Detector`, category **Analysis** (a new category — it processes no audio).
Audio passes through bit-identically; it only observes.

- Detects the musical key (24 candidates: 12 roots x major/minor) with a confidence value.
- Displays the pitch-class profile as a chromagram / note wheel, the detected key, the
  confidence, and the top few alternates.
- Publishes the result so the AI feed can carry it (§4) — the point of the device.

## 2. DSP (JUCE-free, g++-tested) — built for accuracy, not for a quick chromagram

Reference point: Antares Auto-Key is the accuracy bar. Its internals are proprietary and are
NOT to be reverse-engineered or copied — but the published techniques that produce that class
of accuracy are well documented, and two of them explain why it outperforms naive detectors:
**it establishes reference pitch before key**, and it **analyses a committed passage rather
than guessing continuously**. Both are adopted below on their own merits.

A plain FFT chromagram is the thing to avoid: it is what makes most detectors flicker and
fail on dense mixes. The pipeline, in descending order of impact on accuracy:

1. **Tuning estimation FIRST.** Find spectral peaks, measure their deviation from equal
   temperament in cents, histogram it, take the mode → the track's actual reference pitch.
   A track sitting 30 cents sharp smears across pitch-class bins and quietly wrecks every
   downstream step. Report it (`detected_tuning_hz`) as a first-class output, not just an
   input — it is useful in its own right, and it is why this class of detector wins.
2. **Constant-Q transform, not linear FFT.** Log-spaced bins aligned to semitones (typically
   36 bins/octave, i.e. 3 per semitone, ~A1-A7). This is the single biggest quality jump:
   linear FFT bins are far too coarse in the bass and wastefully fine up top, so pitch
   information is smeared exactly where the root lives.
3. **Harmonic/percussive separation.** Median-filter the spectrogram along time (suppresses
   broadband transients) and along frequency (suppresses sustained tones) to split the two;
   keep the harmonic part for analysis. This is what rescues dense, drum-heavy mixes — the
   material where naive detectors confidently report nonsense.
4. **Spectral whitening + harmonic summation.** Flatten the spectral envelope so timbre stops
   dominating, then fold each pitch class's harmonics back onto its fundamental (harmonic
   product / weighted sum over octaves) so a bass note and its overtones reinforce one class
   instead of lighting up several.
5. **Segment-level chroma, then temporal smoothing.** Build a chroma vector per short segment
   (~0.5-1 s), then smooth the SEQUENCE with a Viterbi/HMM pass over key candidates with a
   self-transition bias — rather than correlating one big averaged profile. This is what stops
   relative-major/minor flip-flopping and lets a genuine modulation register as a change.
6. **Key profiles.** Correlate against the 24 candidates using a profile set validated on
   popular music — Temperley's revision or Shaath's, which outperform raw
   Krumhansl-Schmuckler on modern material. Cite whichever is used in a comment. Confidence =
   winning correlation normalised against the runner-up, so "C major 0.9 / A minor 0.88"
   reports as LOW confidence, which is honest.
7. **Hysteresis + `hold`.** Require a margin over the incumbent before switching. `hold`
   freezes the reading outright.

**ANALYSE-on-demand, not a permanently twitching readout.** The primary interaction is:
press ANALYSE (or the AI triggers it), the device listens to the next N seconds of playback,
commits a result, and holds it until re-run. That is both more accurate (a committed passage
beats an instantaneous guess) and more usable. A continuous mode stays available for those
who want it, but the committed reading is what the AI feed reports.

**Tests:** synthesised C-major triad + scale reads C major; A-minor material reads A minor and
specifically NOT C major (the classic relative-key failure); a signal detuned 30 cents is still
identified correctly AND its tuning reported within a few cents; a drum loop and white noise
report LOW confidence rather than a confident wrong answer; a mix with strong percussion still
resolves once HPSS is on (assert it beats the no-HPSS path on the same input); audio passes
through bit-identically at every setting.

## 3. Dialable params (`ParamSchema`, as every device)

| id | unit | range / choices | default | notes |
|---|---|---|---|---|
| `analyse` | on/off | | off | START a committed analysis pass — the primary control; the AI can trigger a re-analysis |
| `window_s` | s | 1-30 | 10 | how much audio a committed pass listens to |
| `continuous` | on/off | | off | keep updating instead of committing once |
| `sensitivity` | % | 0-100 | 50 | how readily it switches key (hysteresis / transition bias) |
| `tuning_hz` | Hz | 415-465 | 440 | reference pitch HINT; `auto_tuning` overrides it |
| `auto_tuning` | on/off | | on | detect the track's reference pitch instead of assuming it (§2.1) |
| `mode_lock` | choice | `auto` \| `major` \| `minor` | `auto` | constrain the search when the user knows |
| `hold` | on/off | | off | freeze the current reading |
| `hpss` | on/off | | on | harmonic/percussive separation — leave on for mixes, off saves CPU on solo tonal sources |
| `low_hz` / `high_hz` | Hz | 20-500 / 1k-12k | 80 / 5000 | analysis band |

Plus `reset` (clear the accumulation). `analyse` being dialable is what lets the model say
"listen again and tell me the key" — an AI-triggered analysis, the same shape as the EQ's
`tame_resonances` action.

**Read-only outputs** (not params — reported, never set): `detected_key`,
`confidence`, `detected_tuning_hz`, `root_hz`, and the top alternates.

## 4. The read path — the new capability

The detected key must reach the model. Follow the existing `[CURRENT CHAIN]` precedent in
`EchoJayAPI` (assembled in `PluginEditor.cpp` alongside the other injections):

```
[DETECTED KEY — from EchoJay Key Detector in the chain; measured from the live signal]:
key: F# minor   confidence: 0.82
detected_tuning: 441.3 Hz (+5 cents from A=440)
root_hz: 92.50 (F#2)   alternates: A major (0.71), C# minor (0.64)
analysed: 10.0 s of playback, committed
```

Rules that keep it honest:
- Emitted **only** when a Key Detector is in the chain and has a reading.
- Always carries **confidence**. The prompt teaching must say: below ~0.5, treat the key as
  unknown and do not build moves on it. A confident wrong key is worse than no key.
- Include `root_hz` so the model can act without doing note maths, and alternates so it can
  see when the call was close (relative major/minor especially).

**Backend teaching** (add to `BUILTIN_DEVICES_BACKEND_SPEC.md`): what the block means, the
confidence rule, and when to use it — see §5.

## 5. What it unlocks (why it is worth building)

The EQ already accepts `note:"G5"` and resolves it via `EqNote.h`'s `parseNoteToFreq`. A
known key plus note-addressable bands makes musical intent directly expressible:

- "notch the ringing at the root" → the model knows the root, sends `note:"F#2"`.
- "cut the boxiness but keep it off the root" → it can avoid the fundamental deliberately.
- "put a dynamic bell on the third" → arithmetic the model can now actually do.
- Delay/reverb choices informed by key and, later, tempo.
- It makes the EQ's `tame_resonances` smarter to interpret: a resonance sitting on the root
  is the instrument; one sitting between scale degrees is more likely a room mode.

## 6. UI

`DeviceEditorBase` as every device. A **note wheel / chromagram** (12 segments, brightness
by pitch-class weight — reuse the dwell-glow treatment from `Source/viz/DwellGlow` so it
matches the family), the detected key large and legible, a confidence meter, and the top
alternates listed small. `HOLD` and `RESET` buttons. Dial row for the params.

## 7. Build notes

- Self-registering like every device — one `BuiltinDeviceRegistrar` line in its own `.cpp`,
  no shared-file edits beyond CMake. New category "Analysis"; add it to the registry's
  `kCategoryOrder` (this IS a shared-line edit — the only one).
- Audio path must be **bit-identical** — prove it in the registry test the way every depth
  pass did (defaults vs explicit-neutral through `processBlock`, zero delta).
- Latency: **zero**. It observes; it never delays.
- CPU: analysis is a few FFTs per second, not per block. Keep it off the audio thread beyond
  filling the ring.
- Device count assertion in `tools/builtin_registry_test.cpp` goes 20 → 21.

## 8. Acceptance

- Play tonal material in a known key: the wheel lights the right classes, the key reads
  correctly, confidence is high. Play a drum loop: confidence collapses rather than
  reporting a confident wrong key.
- `[DETECTED KEY]` appears in the feed only with the device in the chain.
- End to end: with a Key Detector in the chain on material in F# minor, "notch the ringing
  at the root" produces an EQ band at F#'s frequency — the model using an observation the
  plugin made, which is the whole point of the device.
