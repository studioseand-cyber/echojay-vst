# EchoJay Pitch — P0 detection, validation on real vocals

`PITCH_CORRECTION_SPEC.md` §9 gates P0 on proving the detector "on real vocals
across all five `voice_type` settings" and logging the octave-error rate. This
is that log. It was produced with `tools/pitch_probe/` against the same engine
the plugin runs, at the same hop cadence, with nothing resampled.

**Material is described by character, not by path or title.** The takes used
are professional session files from the author's own library; naming them here
would publish what is in that library to anyone who reads this repo, and the
numbers do not need the names to be reproducible.

---

## 1. Why this exists — the synthetic suite was not enough

`test/pitch_engine_test.cpp` reports **0.0% octave-guard fires on all five
voice types**. On real singing the same engine reports **0.23%–8.37%**
depending on material and setting. Sawtooth and simulated vibrato have no
consonants, no breath, no creak, no scoops and no room, so they exercise the
estimator only on the cases it was designed for. Everything below is invisible
to the g++ suite, and all of it would have surfaced later as "PSOLA artefacts".

---

## 2. Results

### 2.1 Solo male rap acapella (118 s, 44.1 kHz, dry, essentially mono)

| voice_type | voiced | guard fires | median conf | largest frame jump | jumps > 600 ¢ |
|---|---|---|---|---|---|
| soprano | 8.6% | 2.67% | 0.844 | 3217 ¢ | 48 |
| **alto_tenor** | **66.3%** | **0.73%** | **0.935** | 2998 ¢ | 112 |
| low_male | 67.5% | 1.67% | 0.930 | 2882 ¢ | 147 |
| instrument | 67.2% | 2.62% | 0.924 | 4213 ¢ | 170 |
| bass | 68.7% | 1.84% | 0.902 | 3866 ¢ | 142 |

### 2.2 Female vocal phrase (13 s, 44.1 kHz, dry mono library sample)

| voice_type | voiced | guard fires | median conf | largest frame jump | jumps > 600 ¢ |
|---|---|---|---|---|---|
| soprano | 78.4% | 0.89% | 0.992 | 1927 ¢ | 22 |
| alto_tenor | 78.3% | 0.92% | 0.988 | 1905 ¢ | 9 |
| low_male | 78.7% | 0.59% | 0.974 | 1206 ¢ | 5 |
| instrument | 78.4% | 0.68% | 0.984 | 1899 ¢ | 7 |
| bass | 78.7% | 0.23% | 0.963 | 1331 ¢ | 3 |

### 2.3 Wide/doubled vocal stem (178 s, 44.1 kHz) — OUT OF SCOPE, kept as a control

| voice_type | voiced | guard fires | median conf | largest frame jump | jumps > 600 ¢ |
|---|---|---|---|---|---|
| soprano | 51.1% | 4.77% | 0.895 | 2438 ¢ | 482 |
| alto_tenor | 61.0% | 7.98% | 0.891 | 3778 ¢ | 831 |
| low_male | 61.3% | 8.37% | 0.873 | 3786 ¢ | 785 |
| instrument | 60.4% | 7.74% | 0.877 | 4284 ¢ | 801 |
| bass | 59.3% | 7.34% | 0.848 | 3893 ¢ | 524 |

Left and right differ on **82%** of samples (mean |L−R| 251 against mean |L|
500): this is a doubled or stereo-widened vocal, so the engine's mid downmix
sums **two simultaneous performances**. The spec scopes the device to
monophonic sources (§1), so this is not a detector defect — it is the detector
correctly failing to find one pitch where there are two. It is kept here
because it is the useful negative control: **no `voice_type` rescues it**,
which is how unsuitable material is told apart from a wrong window.

---

## 3. What the numbers say

**The guard-rate metric works as the spec intends.** On the clean male take the
fire rate tracks how well the window fits the voice: 0.73% at `alto_tenor`
(its true register), rising to 2.6–2.7% at `soprano` and `instrument`. On
material where the window is simply wrong the rate goes up and the voiced
fraction collapses (`soprano` on a male voice: 8.6% voiced). So "if it fires
constantly the window is wrong for the material" holds, and the readout is
diagnostic rather than decorative.

**At the correct `voice_type`, P0 passes the spec's bar.** 0.73% and 0.23%
guard fires on the male and female takes are comfortably inside the couple of
percent the spec allows.

### 3.1 The finding: the guard is blind to non-octave harmonic confusions

Grouping every >600 ¢ single-frame jump on the **clean male take at its correct
`voice_type`** by the frequency ratio of the jump:

| ratio | interval | count |
|---|---|---|
| ×2.00 | octave | 21 |
| **×3.03** | **twelfth (3rd harmonic)** | **20** |
| ×2.93 | twelfth | 4 |
| ×4.92 | ×5 (5th harmonic) | 5 |
| ×1.80, ×1.68 | — | 11 |
| ×1.52 | perfect fifth (×3/2) | 5 |
| ×1.41 | tritone | 5 |

`PitchEngine::analyseHop` evaluates exactly three candidates — `tau/2`, `tau`,
`tau*2`. **Third-harmonic confusions are as frequent as octave errors on real
singing, and the guard cannot see them at all**, because ×3 and ×1.5 are not in
its candidate set. The name "octave guard" turns out to describe its coverage
literally.

Residual rate at the correct `voice_type`: 112 uncaught >600 ¢ jumps over
29,190 voiced hops (0.38%), across 118 s — roughly **one multi-hundred-cent
glitch per second**. Each one is a frame where a corrector would pull the note
to the wrong target, and at P1 that is indistinguishable from a PSOLA bug.

### 3.2 Secondary: parabolic refinement can leave the advertised range

The refinement stage interpolates around the chosen lag with no re-clamp, so a
lag pinned at `tauMin` can refine *below* it. On the `low_male` run (ceiling
500 Hz) 99 voiced hops (0.24%) report f0 **above 500 Hz**, peaking near 521 Hz.
Small, but `voice_type`'s advertised range is a contract the model reasons
about, and a detector that reports outside its own range breaks it.

---

## 4. Recommended before P1

1. **Widen the guard's candidate set to `tau/3`, `tau/2`, `tau*2`, `tau*3`**
   (and evaluate ×3/2), keeping the existing asymmetry rule: a sub-multiple
   claim must show real CMNDF evidence, a super-multiple may win on continuity.
   This is the single highest-value change, and §3.1 is the evidence for it.
2. **Clamp the refined lag to `[tauMin, tauMax]`** so a reported f0 can never
   leave the advertised `voice_type` range.
3. Consider rejecting a hop whose f0 jumps more than ~600 ¢ from a *confident*
   recent median, rather than publishing it — the median-of-3 currently absorbs
   single-hop spikes but not the 3–5 hop runs seen in §2.3.

None of this is P1 work. It is P0 becoming solid, and the spec is explicit that
P0 must be solid before anything is built on it.
