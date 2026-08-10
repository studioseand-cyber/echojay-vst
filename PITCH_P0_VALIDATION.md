# EchoJay Pitch — P0 detection: validation on real vocals, and the two repairs

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

At the time of measurement `PitchEngine::analyseHop` evaluated exactly three
candidates — `tau/2`, `tau`, `tau*2`. **Third-harmonic confusions are as
frequent as octave errors on real singing, and that guard could not see them at
all**, because ×3 and ×1.5 were not in its candidate set. The name "octave
guard" turned out to describe its coverage literally. §4 is the repair.

Residual rate at the correct `voice_type`: 112 uncaught >600 ¢ jumps over
29,190 voiced hops (0.38%), across 118 s — roughly **one multi-hundred-cent
glitch per second**. Each one is a frame where a corrector would pull the note
to the wrong target, and at P1 that is indistinguishable from a PSOLA bug.

### 3.2 Secondary: parabolic refinement can leave the advertised range

The refinement stage interpolated around the chosen lag with no re-clamp, so a
lag pinned at `tauMin` could refine *below* it. On the `low_male` run (ceiling
500 Hz) 99 voiced hops (0.24%) reported f0 **above 500 Hz**, peaking near
521 Hz. Small, but `voice_type`'s advertised range is a contract the model
reasons about, and a detector that reports outside its own range breaks it.
Fixed and pinned by a test in §4.4.

---

## 4. The repairs, and what they bought

Both were applied. Measurement is `RESIDUAL` from the probe: **>600 ¢
single-frame jumps per 1000 voiced hops**, normalised so settings that voice
different numbers of hops compare directly.

### 4.1 Which direction is the error? (this decides the rule)

A **larger tau reports a lower f0**, so `tau*3` claims f0/3 (a sub-third,
downward) and `tau/3` claims 3×f0 (a harmonic lock, upward). Those need
**opposite** treatment, because every signal periodic at T is also periodic at
2T and 3T — the downward direction always ties on aperiodicity, the upward one
does not. One rule applied to both would either reintroduce the sub-harmonic
lock or leave the harmonic lock uncaught.

Measured on the male take, classifying each voiced hop against the median of
its 60 neighbours:

| | count | per 1000 voiced |
|---|---|---|
| outliers reporting too **HIGH** (harmonic lock) | 324 | 11.10 |
| outliers reporting too **LOW** (sub-harmonic) | 160 | 5.48 |

Harmonic locks outnumber sub-harmonic ones roughly 2:1, so the correction that
matters most moves the estimate **down** — which is the evidence-gated
direction, and therefore the safe one to widen.

### 4.2 The lattice, and whether ×1.5 earns its place

Shipped candidate set: `{tau/3, tau/2, tau*2/3, tau, tau*3/2, tau*2, tau*3}`.
Upward claims (shorter lag) may win on continuity plus a small margin; downward
claims (longer lag) must additionally show CMNDF evidence of a present-but-weak
fundamental.

A/B against the same set without the two fifth candidates (`tau*2/3`,
`tau*3/2`), residual per 1000 voiced hops:

| material | before | with ×1.5 | without ×1.5 |
|---|---|---|---|
| male, all five types (hop-weighted) | 5.01 | **4.59** (−8.4%) | 4.68 (−6.6%) |
| male at its correct `alto_tenor` | 3.84 | **3.39** (−11.7%) | 3.56 (−7.3%) |
| female, all five types | 2.40 | **1.15** (−52.1%) | 1.25 (−47.8%) |
| doubled stem (control) | 17.39 | **14.95** (−14.0%) | 15.38 (−11.5%) |

**Verdict: ×1.5 is kept, but the honest magnitude is small.** It is never worse
in any of the 15 file×voice_type cells and better in 8, which is the evidence
that it reduces real errors rather than adding false corrections — but it
accounts for only about a fifth of the total improvement (0.09 of the male
file's 0.42), and on absolute terms buys ~5 fewer bad hops out of 29,190 at the
correct setting. It survives on consistency, not on size. If the candidate
count ever needs cutting, this is the pair to cut first.

### 4.3 Full before/after

Residual >600 ¢ jumps per 1000 voiced hops:

| voice_type | male before | male after | female before | female after |
|---|---|---|---|---|
| soprano | 12.67 | 12.14 | 5.77 | 1.05 |
| alto_tenor | 3.84 | **3.39** | 2.37 | 1.31 |
| low_male | 4.86 | 4.56 | 1.29 | 1.29 |
| instrument | 5.75 | 5.48 | 1.84 | 1.32 |
| bass | 4.62 | 3.97 | 0.78 | **0.78** |

**Guard fire rate rises** as a direct consequence — on the male take
`alto_tenor` goes 0.73% → 0.90%, `low_male` 1.67% → 2.07%. That is expected and
is not a regression: the extra fires are the ×3 corrections that were
previously impossible, and the residual falls at the same time. It does mean
the probe's "over 2%" annotation is now a coarser signal than it was, since a
wider lattice legitimately fires more; read it together with `RESIDUAL`, not
alone.

### 4.4 Range clamp

`refineTau` now clamps into `[tauMin, tauMax]`, and the published f0 is clamped
to the active type's `[fMin, fMax]` (the integer lag grid straddles the
advertised bounds by a fraction of a bin). `test/pitch_engine_test.cpp` drives
every voice type with material deliberately outside its range — 3× the ceiling,
a third of the floor, and sine and sawtooth at each boundary — and fails if any
voiced hop reports outside. Against the pre-fix engine that test fails on 4 of
5 types (soprano 1400.66 Hz, instrument 2003.63 Hz, low_male and bass 500.37
Hz), so it is a test that bites rather than a vacuous one.

### 4.5 Regression guard

The synthetic suite is what stops the wider lattice buying real-vocal accuracy
by corrupting clean tones. It is now **63 checks, all passing** (58 before, plus
the 5 range checks), and the per-voice-type fire-rate assertion was tightened
from "under 10%" to **exactly zero**: the guard fires 0 times on clean in-range
vibrato material at all five settings, unchanged by the lattice.

### 4.6 Still open

Rejecting a hop whose f0 jumps more than ~600 ¢ from a *confident* recent
median was considered and **not** done. The median-of-3 absorbs single-hop
spikes but not the 3–5 hop runs in §2.3, and a longer median trades tracking
latency for stability — that is a retune-envelope question (P2), not a detector
one, and it should be decided with the envelope in front of us rather than
guessed at now.
