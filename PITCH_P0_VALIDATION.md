# EchoJay Pitch — the measurement record (P0 detection, P1 shifting)

`PITCH_CORRECTION_SPEC.md` §9 gates P0 on proving the detector "on real vocals
across all five `voice_type` settings" and logging the octave-error rate. This
is that log. It was produced with `tools/pitch_probe/` against the same engine
the plugin runs, at the same hop cadence, with nothing resampled.

**Material is described by character, not by path or title.** The takes used
are professional session files from the author's own library; naming them here
would publish what is in that library to anyone who reads this repo, and the
numbers do not need the names to be reproducible.

---

## 0. THE DETECTION FLOOR — read this before debugging a P1 artefact

**At the shipped default (`tracking = normal`) the detector emits a wrong pitch
roughly every 1.5 to 2.6 seconds of tracked singing.** Measured on a real solo
male acapella, all figures per 1000 tracked hops:

| at `tracking = normal` | residual >600 ¢ | one every | of which HIGH-confidence + mid-phrase |
|---|---|---|---|
| `alto_tenor` | **1.81** | ~2.6 s | 0.06 (1 event in 118 s) |
| `low_male` | **2.92** | ~1.6 s | 0.30 (5 events in 118 s) |

Two different numbers, and the distinction is the whole point:

- **The total rate** is what you will actually hear — roughly one glitch every
  two seconds. Most of it is at consonants and phrase edges, where a corrector
  should be doing little anyway.
- **The high-confidence mid-phrase subset** is the irreducible part: frames that
  look perfectly healthy — confident, mid-phrase, steady-state — and are simply
  wrong. It runs 10–30× rarer than the total, about one every 20–120 seconds. No
  threshold reaches it, because by construction those frames are confident.

**Why this is written down as a number.** During P1 someone will hear a wrong
note every couple of seconds, reach for the shifter, and spend a day inside
PSOLA. That day is the entire reason P0 was gated separately. Before debugging
any downstream artefact, check the detector first: `tools/pitch_probe --csv`
runs the same engine the plugin does and shows exactly which frame was
mistracked, with its confidence.

**The rule of thumb:** an artefact at roughly one per two seconds, clustered at
consonants and phrase edges, is presumed **detection** until proven otherwise.
An artefact that is denser than that, or that lands mid-vowel on confident
frames, is **downstream**.

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

---

## 5. What the REMAINING residuals actually are

Before widening the candidate set any further, the residual was characterised
rather than attacked. The question is whether the surviving jumps are
mis-estimates (fixable by better candidate search) or genuinely ambiguous
frames (fixable only by declining to trust them).

Method: on the male take at `alto_tenor` and `low_male`, every remaining >600 ¢
adjacent-hop jump was logged with its own confidence, the mean confidence of
the five frames either side, its input RMS, and whether it sits within 50 ms of
a voiced onset or offset. Every figure is quoted **against the base rate over
all voiced frames**, because "most jumps are low-confidence" means nothing if
most frames are low-confidence.

### 5.1 The distribution

| | jump frames | all voiced | |
|---|---|---|---|
| **alto_tenor** — median confidence | 0.701 | 0.935 | −0.234 |
| local confidence (5 either side) | 0.756 | 0.932 | −0.175 |
| input RMS | −16.6 dB | −13.1 dB | −3.5 dB |
| within 50 ms of onset/offset | **85.9%** | 26.4% | **3.25×** |
| **low_male** — median confidence | 0.701 | 0.931 | −0.230 |
| local confidence (5 either side) | 0.736 | 0.928 | −0.192 |
| input RMS | −15.3 dB | −13.2 dB | −2.1 dB |
| within 50 ms of onset/offset | **76.8%** | 25.1% | **3.06×** |

Confidence-band enrichment (`alto_tenor`; `low_male` is within a point of it):

| confidence | share of jumps | share of voiced | enrichment |
|---|---|---|---|
| 0.60–0.70 | **49.5%** | 8.1% | **6.11×** |
| 0.70–0.80 | 35.4% | 10.6% | 3.33× |
| 0.80–0.90 | 9.1% | 19.3% | 0.47× |
| 0.90–0.95 | 5.1% | 18.6% | 0.27× |
| 0.95–1.00 | 1.0% | 43.4% | 0.02× |

Nothing appears below 0.60 because the current voiced gate already sits there
(`kVoicedAperiodicity = 0.40` ⇒ confidence ≥ 0.60). **The residuals pile up in
the narrow band immediately above the existing gate.**

Creak shows up exactly where the search range can represent it: with an f0
below 0.6× the take's median, creak-like frames are **14.5% of jumps against a
1.8% base rate at `low_male`** (8×), and **0.0% at `alto_tenor`** — whose 80 Hz
floor cannot represent creak at all, so those frames simply read unvoiced.

### 5.2 The hypothesis holds — with a residue that does not

Classifying every jump by confidence (<0.85), boundary proximity, and creak:

| class | alto_tenor | low_male |
|---|---|---|
| low-confidence, at a boundary | 82.8% | 73.1% |
| low-confidence, mid-phrase | 10.1% | 16.0% |
| **high-confidence, mid-phrase** | **4.0%** | **7.3%** |
| high-confidence, at a boundary | 3.0% | 3.6% |

**Roughly 93% (alto_tenor) and 89% (low_male) of residuals are low-confidence,
and 77–86% sit within 50 ms of an onset or offset.** The hypothesis is
confirmed: these are frames where the pitch is genuinely ambiguous, not frames
the estimator got wrong.

The honest residue: **4.0% and 7.3% are high-confidence and mid-phrase.** Their
rate among high-confidence mid-phrase non-creak frames is **0.22 and 0.37 per
1000**, against overall rates of 3.39 and 4.56 — **12–16× lower**. So it is not
a flat distribution hiding behind an average, but it is not zero either: a
handful of genuine mid-phrase mis-estimates survive, and no confidence gate
will reach them.

### 5.3 Proposed threshold, and what it costs

Simulated by gating an existing run and recomputing adjacency. **These numbers
turned out to be optimistic — see the correction in §6.3.** They are kept
because the methodological lesson is worth more than the estimate was:

| gate | alto_tenor residual /1000 | vs now | low_male residual /1000 | vs now | frames tracked before that become passthrough |
|---|---|---|---|---|---|
| 0.60 (current) | 3.39 | — | 4.56 | — | — |
| 0.70 | 1.16 | −66% | 2.13 | −53% | 8.1% / 8.5% |
| **0.75** | **0.79** | **−77%** | **1.65** | **−64%** | **12.7% / 13.8%** |
| 0.80 | 0.51 | −85% | 0.87 | −81% | 18.7% / 20.5% |
| 0.85 | 0.33 | −90% | 0.70 | −85% | 26.4% / 28.7% |

**Proposal: confidence ≥ 0.75.** That is the knee — 0.70→0.75 buys 11 points of reduction for 4.6 points of
tracking, while 0.75→0.80 buys 8 points for 6. The voiced share of all hops
falls from 66.3% to 57.9% (`alto_tenor`).

**And the cost really is close to nothing**, which is worth showing rather than
asserting. Of the frames a 0.75 gate drops, **80% (alto_tenor) and 76%
(low_male) are within 50 ms of a boundary** — consonants, breath and release
tails, where a corrector should not be acting anyway. The mid-phrase gaps it
opens are **median 11 ms**, and the longest is 59 ms at `alto_tenor`. At
`low_male` there is exactly **one** mid-phrase gap over 100 ms (142 ms), which
is the only case a P2 retune envelope would have to bridge deliberately.

One qualification on "gating costs nothing": it is free for *correctness* —
a frame we decline to trust never drives correction, and unvoiced frames pass
through untouched by design. It is not quite free for *continuity*, because
13% of currently-tracked frames stop being corrected, and at P2 the retune
envelope must hold its target across those gaps rather than resetting. At a
median 11 ms that is well inside every `retune_speed_ms` in the spec's table,
so it should be a non-event — but it is an envelope requirement, not an
automatic consequence.

---

## 6. `tracking` — the gate, made dialable

The threshold is not hardcoded. It is the `tracking` parameter of §5 of the
spec, and all three settings are **measured rather than chosen**:

| `tracking` | confidence floor | what it is |
|---|---|---|
| `relaxed` | 0.60 | the pre-gate behaviour, unchanged. For breathy or quiet sources where losing frames costs more than the odd bad one. |
| **`normal`** (default) | **0.75** | the knee of §5.3 — 77% / 64% of residual removed for ~13% of tracked frames. |
| `tight` | 0.80 | the ceiling set by **gap length**, not by residual. |

### 6.1 Why `tight` is 0.80 and not a rounder, higher number

`tight` is bounded by what the retune envelope can be asked to bridge, so it was
picked against a measured ceiling: **the highest threshold at which fewer than
2% of mid-phrase gaps exceed 100 ms.** Gap = a run of frames that were voiced
but are dropped by the gate, bounded on both sides by tracked frames — exactly
what the envelope must hold its target across.

| gate | >100 ms gaps: alto_tenor / low_male / female | p95 gap (ms) |
|---|---|---|
| 0.75 | 0% / 0.6% / 0% | 40 / 45 / 8 |
| **0.80** | **1.6% / 1.1% / 0%** | **59 / 55 / 24** |
| 0.82 | 2.8% / 2.6% / 0% | 72 / 68 / 40 |
| 0.85 | 2.2% / 5.6% / 0% | 80 / 111 / 48 |

(Gap figures are simulated, and safe to simulate: the gating DECISIONS match
the live engine to within 0.07% — 25,473 predicted against 25,456 actual
tracked frames. What simulation gets wrong is the f0 VALUES, which is §6.3.)

0.82 is where the >100 ms share doubles past 2%, and by 0.85 the `low_male` p95
gap itself breaks 100 ms. The residual gain beyond 0.80 is small and buys gaps
the envelope then has to reason about, so 0.80 is the stop.

A note on the statistic: on a **maximum**-gap reading no threshold above 0.60
qualifies — even `normal` has a single 142 ms outlier. The ceiling is therefore
applied to the distribution (p95, and the share over 100 ms), and the worst
observed case is carried into the spec's §2.3 envelope requirement explicitly
rather than being averaged away.

### 6.2 What this buys P1 — MEASURED, on the shipped engine

| `tracking` | alto_tenor: tracked / residual | low_male: tracked / residual |
|---|---|---|
| `relaxed` | 66.3% / 3.39 | 67.5% / 4.56 |
| **`normal`** | **57.8% / 1.81** (−47%) | **58.2% / 2.92** (−36%) |
| `tight` | 53.9% / 1.73 (−49%) | 53.6% / 2.12 (−54%) |

`relaxed` reproduces the pre-gate numbers exactly (3.39 / 4.56), which is the
check that the structural floor and the relaxed floor really are the same line.

P1 therefore develops against roughly **one detection glitch every 1.6–2.6
seconds** instead of one every 1.2 seconds. A real improvement, and about half
what the §5.3 simulation promised.

### 6.3 Why the simulation over-promised — a methodological correction

§5.3 predicted 0.79 / 1.65. The shipped engine delivers 1.81 / 2.92. The
simulation was wrong by roughly 2×, and the reason is worth recording because
it will recur:

**Simulate-by-deletion assumes the estimator is stateless. It is not.** Deleting
low-confidence frames from a completed run only removes those frames. Gating
them in the live engine also changes what the estimator produces *afterwards*,
because two pieces of state advance only on tracked frames — the median-of-3
history and the continuity reference the guard scores candidates against.

Measured directly, comparing the `relaxed` and `normal` runs hop by hop:

- 25,456 hops are voiced in both runs (the gating decisions agree),
- but **306 of them (1.20%) carry a different f0**, and **57 differ by more
  than 20%**.

Those 57 are more than enough to account for the gap between 20 predicted jumps
and 46 actual ones. The lesson: a gate on a stateful estimator has to be
measured end to end, never simulated from a prior run's output.

A second, smaller correction came out of the same measurement. The gate was
first wired to the *entry* check, which shares its threshold with the guard's
candidate-sanity test — so tightening `tracking` quietly **disabled guard
corrections**, rejecting a good sub-multiple candidate for being less periodic
than the gate allowed and publishing the known-wrong raw answer in its place
(1.96 / 3.06). The gate now decides **whether to publish**, applied to the final
chosen candidate, while the guard keeps a fixed structural bound
(`kMaxAperiodicity`). A frame whose corrected value is not trusted is reported
untracked rather than reported wrong.

**The §5.3 proposal is now implemented as `tracking`.** What remains unimplemented
is the median-based outlier rejection of §4.6, which is still a P2 envelope
question.


---

## 7. P1 latency budget — and the part that is physics

`120 ms` at `bass` is a long way past "a few ms slower", so here is where it
actually goes.

### 7.1 The breakdown is simpler than expected: it is ALL shifter

| stage | contributes to LATENCY? | why |
|---|---|---|
| detector analysis window (2.5 periods + one more for `tauMax`) | **none** | It is a look-BACK. The window ends at the current sample, so the estimate for sample *p* is available at *p*. Nothing waits on it. |
| PSOLA lookahead | **all of it** | A grain is centred on an epoch and reaches one period FORWARD, and the epoch itself has to be found before the grain can be cut. That is future audio, and future audio is latency. |

So the analysis window costs nothing in delay. What it does cost is **tracking
lag**: the f0 attached to sample *p* describes the 3.5 periods *ending* at *p*,
so on a moving pitch it is roughly half a window stale — 15 ms at `alto_tenor`,
70 ms at `bass`. That is a P2 retune-envelope problem, not a monitoring one,
but it is the other half of the same window and worth naming before P2 meets it.

### 7.2 Current, and what is actually reachable (48 kHz)

Latency is `lookahead x period(fMin)`. The shipped lookahead is **3 periods**,
which measurement says is conservative:

| `voice_type` | fMin | period | **shipped (3T)** | at 2T | at 1.25T |
|---|---|---|---|---|---|
| soprano | 180 Hz | 267 | **16.7 ms** | 11.1 ms | 7.0 ms |
| alto_tenor | 80 Hz | 600 | **37.5 ms** | 25.0 ms | 15.6 ms |
| low_male | 55 Hz | 873 | **54.6 ms** | 36.4 ms | 22.7 ms |
| instrument | 50 Hz | 960 | **60.0 ms** | 40.0 ms | 25.0 ms |
| bass | 25 Hz | 1920 | **120.0 ms** | 80.0 ms | 50.0 ms |

The reduction was measured, not assumed. Sweeping the lookahead with the
shipped margin structure, the 30-check PSOLA suite passes at every setting from
3.0 down to 1.1 periods, and on the **real 118 s acapella** shifted to a fixed
220 Hz the output barely moves:

| lookahead | detected median | 5–95 spread |
|---|---|---|
| 3.00 T (shipped) | +0.8 ¢ | 48.3 ¢ |
| 2.00 T | +0.7 ¢ | 48.5 ¢ |
| 1.50 T | +0.8 ¢ | 52.2 ¢ |
| 1.25 T | +0.7 ¢ | 47.1 ¢ |
| 1.10 T | +0.8 ¢ | 45.8 ¢ |

**Caveat that matters:** these are pitch-accuracy measures. They would not
catch transient smearing at onsets, which is exactly what a shorter lookahead
threatens. Cutting the shipped 3T is therefore a change to make *after* a
listen, not before one — the numbers say it is available, not that it is free.

### 7.3 The physics, and what it means for `low_latency`

**PSOLA lookahead is proportional to the period of the lowest pitch the window
must represent.** That is not an implementation detail that can be optimised
away: you cannot window one period of a 25 Hz tone without holding 40 ms of
audio, because the period *is* 40 ms. `low_latency` therefore has exactly two
levers, and only one of them is free:

1. **Shorten the lookahead multiplier** (3T → ~1.25T). Costs nothing measurable
   so far; needs a listen.
2. **Raise the search floor**, which is what the spec already describes as
   trading low-note accuracy for delay. Raising `bass` from 25 Hz to 40 Hz
   nearly halves its period.

Both together, the best case per type:

| `voice_type` | shipped | `low_latency` best case | monitorable? |
|---|---|---|---|
| soprano | 16.7 ms | **7.0 ms** | yes |
| alto_tenor | 37.5 ms | **12.5 ms** (floor 100 Hz) | marginal |
| low_male | 54.6 ms | **15.0 ms** (floor 80 Hz) | marginal |
| instrument | 60.0 ms | **18.8 ms** (floor 80 Hz) | marginal |
| bass | 120.0 ms | **31.3 ms** (floor 40 Hz) | **no** |

Singers monitoring themselves are less tolerant of delay than instrumentalists,
because they also hear their own voice by bone conduction and the two combine —
roughly 10 ms is comfortable and past ~20 ms it interferes with performing.

**The conclusion, stated plainly: a bass singer cannot monitor through this
device, and no amount of engineering changes that** — a 25 Hz floor means a
40 ms period, and one period is the floor of what epoch-synchronous windowing
can work with. The upper three types *can* be brought into or near monitoring
range, and only by doing both levers at once.

### 7.4 So does `low_latency` move up the plan?

**It moves up if tracking vocals through the plugin is a target use case, and
stays at P5 if it is not.** The spec's §9 ordering assumes mixing, where 37 ms
of reported, compensated latency is a non-event. The moment someone wants to
sing through it, `low_latency` stops being a nicety and becomes the difference
between usable and not — for four of the five voice types. For `bass` it is the
difference between unusable and still unusable.

Recommendation: decide the use case now rather than at P5. If monitoring is in
scope, the cheap half (the multiplier) should land with P2 while the shifter is
still being listened to, since that is when a regression would be caught.

---

## 8. `low_latency`: measuring the thing at risk, not the thing that is easy

§7.2 swept the lookahead on **pitch accuracy** and found it flat from 3.0 down
to 1.1 periods. That measurement could not see the risk. A shorter lookahead
threatens **onset transients**, and pitch accuracy is blind to smearing.

So the measurement was redone on what is actually at stake: at each of 308 real
voiced onsets in the acapella, the **10%→90% attack rise time** and the
**peak-to-RMS over the first 20 ms**, processed against dry, while shifting up
a semitone.

| lookahead | Δ rise vs dry | Δ crest vs dry |
|---|---|---|
| 3.00 T (shipped) | +0.837 ms | +0.17 dB |
| 2.50 T | +0.916 | +0.19 |
| 2.00 T | +1.039 | +0.18 |
| 1.75 T | +1.120 | +0.20 |
| 1.50 T | +1.419 | +0.30 |
| 1.25 T | +1.600 | +0.56 |
| 1.10 T | +2.119 | +0.99 |

Degradation is monotonic and the knee is around **1.5**, not 1.1 — the pitch
sweep was measuring the wrong thing.

### 8.1 The tracking-lag fix turned out to matter more than the multiplier

The same harness, with the §7.1 timestamp compensation switched on:

| lookahead | Δ rise vs dry | Δ crest vs dry |
|---|---|---|
| **3.00 T** | **−0.248 ms** | **+0.00 dB** |
| 2.50 T | +0.171 | +0.26 |
| **2.00 T** | **−0.238** | **+0.63** |
| 1.75 T | +0.685 | +1.30 |
| 1.50 T | +0.660 | +1.50 |

At the shipped lookahead, aligning the f0 to the audio it describes takes onset
crest to **exactly the dry value** and the attack rise to slightly *faster* than
dry. The onset smearing attributed to PSOLA was mostly the shifter cutting
grains with a **stale period**, not the windowing.

It also confirms the floor from a second direction. The compensation is
1.75 periods, and it must land ahead of the shifter's read point, so
**lookahead ≥ ~1.75 periods** or the compensation gets truncated — which is
exactly where crest starts diverging (+1.30 dB at 1.75, +1.50 at 1.50). Two
independent constraints, the same answer.

### 8.2 What shipped

`low_latency` moves the multiplier only, 3.0 → **2.0 periods**: rise stays
better than dry (−0.238 ms) and crest gives up 0.63 dB. 1.75 was rejected — it
sits on the compensation floor with no margin.

| `voice_type` | MIXING (3T) | TRACKING (2T) |
|---|---|---|
| soprano | 16.7 ms | **11.1 ms** |
| alto_tenor | 37.5 ms | **25.0 ms** |
| low_male | 54.6 ms | **36.4 ms** |
| instrument | 60.0 ms | **40.0 ms** |
| bass | 120.0 ms | **80.0 ms** |

The search-floor lever is **not** touched — it changes what the detector can
represent and interacts with the creak finding of §5.1, and only one lever moves
while a listening loop is open.

It is a **mode button**, labelled MIXING/TRACKING with the resulting latency for
the current `voice_type` printed on it, and the cost stated beside it. It is
**manual by construction**: nothing auto-switches it from transport or
record-arm state, because changing reported latency forces the host to rebuild
delay compensation, and doing that at the instant record engages is the worst
possible moment for it.

---

## 9. P2 correction, measured

The corrector is measured **in isolation from the shifter**, feeding the real
take's detected f0 through `PitchCorrect` and comparing distance to the nearest
enabled degree, in against out:

| setting | input | output |
|---|---|---|
| hard, chromatic (retune 0, flex 0) | 13.0 ¢ | **0.0 ¢** |
| natural, chromatic (120 / 55 / 60) | 13.0 ¢ | **9.5 ¢** |
| hard, forced to C minor | 81.9 ¢ | **0.0 ¢** |
| natural, forced to C minor | 81.9 ¢ | 46.4 ¢ |

Measured at the rendered **audio** instead, the hard character reads 7.5 ¢ mean
deviation rather than 0. **That mean is misleading and the first reading of it
here was wrong** — see §9.1. It is a long tail, and the tail is the detector
measuring, not the shifter landing.

| render | mean dev | within 5 ¢ |
|---|---|---|
| dry | 13.0 ¢ | 35.3% |
| **hard tuned** | **7.5 ¢** | **60.6%** |
| **natural** | **11.8 ¢** | 34.5% |
| wrong key (C minor forced) | 29.7 ¢ | 4.9% |

(`natural` also carries a **+3.2 ¢ systematic bias** against hard tune's +0.3 ¢.
Small, but it is the retune envelope trailing a moving pitch rather than noise,
and worth remembering if transparent mode ever reads faintly sharp.)

### 9.1 The 7.5 ¢ is a TAIL, not a spread — and mostly measurement

A mean cannot answer "is hard tune locked", because the same detector whose
error floor §0 records is the instrument doing the measuring. The distribution
separates them.

**Measurement floor first.** The same detector on signals that are exactly on
pitch by construction:

| control | p50 | p95 |
|---|---|---|
| synthetic saw at 220.000 Hz (A3 exactly) | 0.3 ¢ | 0.3 ¢ |
| + noise at realistic breathy SNR | 0.4 ¢ | 0.7 ¢ |
| + heavier noise | 0.6 ¢ | 1.6 ¢ |

On clean material the detector is essentially exact, so anything large is either
the shifter or the detector struggling with real material.

**The renders, all voiced frames:**

| | mean | p50 | p75 | p90 | p95 | p99 |
|---|---|---|---|---|---|---|
| dry | 13.0 ¢ | 8.3 | 18.6 | 34.4 | 41.8 | 48.6 |
| **hard tuned** | 7.5 ¢ | **3.6** | **8.4** | 20.1 | 32.1 | 46.0 |
| natural | 11.8 ¢ | 8.2 | 16.3 | 29.4 | 37.1 | 47.7 |

**The median is 3.6 ¢, not 7.** The mean is dragged by a tail.

**And the tail is confidence-dependent, which settles it.** Hard tuned,
restricted by the detector's own confidence:

| frames | p50 | p75 | p90 | p95 |
|---|---|---|---|---|
| conf ≥ 0.95 | **2.5 ¢** | **4.9 ¢** | 8.3 | 11.7 |
| conf ≥ 0.90 | 2.9 | 5.7 | 11.4 | 17.8 |
| conf ≥ 0.80 | 3.4 | 7.6 | 17.6 | 30.0 |
| all voiced | 3.6 | 8.4 | 20.1 | 32.1 |

As confidence rises the **tail collapses** (p95 32.1 → 11.7 ¢) while the
**median barely moves** (3.6 → 2.5 ¢). That is the signature of measurement
error concentrated in exactly the frames §5 established the detector is
unreliable on — not of a shifter landing consistently off.

**Verdict: hard tune is locked.** Half of all frames sit within 3.6 ¢ and three
quarters within 8.4 ¢; on frames the detector is confident about, half are
within 2.5 ¢ and three quarters within 4.9 ¢. Systematic bias is +0.3 to +0.8 ¢.

**What is honestly still the shifter:** p99 stays at 25.8 ¢ even at conf ≥ 0.95,
so about one frame in a hundred genuinely lands a quarter-semitone out. That is
a real residual, not measurement, and small enough not to break the locked
character — but it is the number to watch if hard tune ever sounds loose.

The earlier attribution of the whole 7.5 ¢ to "the shifter's accuracy" was
wrong: most of it was the instrument, not the thing being measured.

`natural` sitting close to the dry signal is the point of it, not a failure:
transparent correction should tidy pitch without flattening the performance.

**The wrong-key row is kept deliberately.** Forcing a scale the take is not in
makes correction measure *worse* than doing nothing, because partial correction
toward foreign degrees lands between semitones. Until the key auto-map exists,
**chromatic is the honest default** — it still tunes and it cannot force a note
that is wrong for the song.

---

## 10. P4 — the key auto-map

### 10.1 The precedence walk is not re-implemented

`PluginEditor::collectKeySources()` already ranks key sources once — newest
capture, then a bus Link, then this plugin when its declared role IS a music
bus, then a channel Link, then the local chain (poisoned on a vocal channel) —
and both the `[DETECTED KEY]` feed block and the Meters KEY panel read it.

The device does **not** repeat that walk. The editor publishes its already-
resolved primary into `EedKeyFeed`, and the device reads it. Two rankings that
can disagree is the exact bug the shared collector was written to prevent.

One change was needed on the publisher side: source collection had been gated
on the Meters view being visible. A corrector that only tracks the key while
the user happens to be looking at Meters would be a genuinely baffling bug, so
the 2 Hz collection now runs regardless of view and the Meters branch just
consumes the cache.

### 10.2 The confidence gate falls to chromatic, never to the last key

Below 0.50 the key is **not applied** and the scale falls back to chromatic.
Not to the previous key: a stale key is applied with total confidence and can
force a note that is wrong for the song, where chromatic still tunes every note
and cannot. The number that justifies this is §9's — correcting to a wrong key
pushed a take from 13.0 ¢ off the nearest note to **29.7 ¢**, i.e. worse than
not correcting at all.

The fallback is **shown**, in amber, naming the confidence that was rejected.
A user who cannot see that the key was refused reads chromatic correction as
the device misbehaving.

### 10.3 Three defects the P4 tests caught

**Constructed defaults disagreed with advertised defaults.** `correct`
advertised on but constructed off; `correction_mode` advertised natural but
displayed custom; and `scale` advertised chromatic but constructed **major** —
so a freshly inserted device came up forcing C major, which is precisely the
guess §9 and the advertisement forbid. A state restore would have hidden it
forever after, since restore writes the schema defaults; only the first
instance was wrong. There is now a test that walks every ParamSpec and compares
its default against a freshly constructed device.

**The scale cross-fade froze during silence.** It advanced only on voiced
frames, and a key change most often lands in the gap between phrases — exactly
where it would sit stuck. It now advances unconditionally.

**`getSampleRate()` can be zero.** The corrector's time step was
`1000 * n / getSampleRate()`, but that rate is set by the *host* via
`setRateAndBufferSizeDetails`, not by `prepareToPlay`. Any path that prepares
directly leaves it zero, making the time step infinite and silently completing
every millisecond constant in the corrector on its first block. It now uses the
rate the device was actually prepared with.

### 10.4 Live key changes

A modulation cross-fades the scale over 300 ms rather than switching on a
sample, because a hard scale switch under a sustained note is audible. The fade
blends the resulting **targets**, not the degree masks — a half-enabled degree
is meaningless, whereas a target travelling from where the old scale put the
note to where the new one does is the audible behaviour actually wanted.

### 10.5 What is reused rather than rebuilt

The two-tier precondition of `KEY_PRECONDITION_SPEC.md` already fires on
"autotune", "pitch correction" and similar. Its needle list gained the phrases
that name this device or plainly ask for tuning. No second prompt was written:
Tier 1 still adds a Key Detector to an existing bus Link itself, and Tier 2 —
the only thing a user must do, placing a Link — remains the server
classifier's.

---

## 11. Defaults across the whole suite, and what P5 rejected

### 11.1 Advertised vs constructed defaults — all 22 devices

The Pitch defaults bug of §10.3 was not device-specific, so the check was
generalised: construct every registered built-in through the same factory the
chain uses and compare each `ParamSpec.def` against `getParamValue`.

**Seven mismatches, across four devices, all in Modulation:**

| device | mismatch |
|---|---|
| EchoJay Auto Pan | `rate_hz` advertised 0.5, constructs 1.0; `depth` advertised 70, constructs 50 |
| EchoJay Chorus | `rate_hz` advertised 0.6, constructs 1.0; `depth` advertised 40, constructs 50 |
| EchoJay Phaser | `rate_hz` advertised 0.3, constructs 1.0; `depth` advertised 70, constructs 50 |
| EchoJay Tremolo | `rate_hz` advertised 4.0, constructs 1.0 |

One cause: they share an LFO core whose members construct at the core's generic
1 Hz / 50%, while each device advertises its own musically-chosen default. So a
freshly inserted Chorus **runs** at 1 Hz / 50% while the model believes it is at
0.6 Hz / 40%, and any dialling relative to "the default" starts from a false
premise. The error then hides itself: a state restore writes the schema
defaults, so only the very first instance is ever wrong.

**FIXED**, at the funnel: `BuiltinDeviceRegistry::add` now wraps every device's
`create` so a newly constructed device gets `resetParamsToDefaults()`, writing
every schema default through `setParamValue` for all 22 at once. Fixing four
sets of member initialisers instead would have left the trap open for device 23.
All 22 now construct at their advertised defaults and the check is pinned at
**zero**, so any mismatch is a regression from here.

Two side effects had to be guarded, and both were the same shape: a defaults
write is not a user's hand on a knob. `resetParamsToDefaults` is now virtual so
EchoJay Pitch can suppress the rules that flip `key_source` to manual when
`key_root`/`scale` is set, and that knock `correction_mode` to custom when
retune/flex/humanize move — otherwise writing the defaults left two params
contradicting their own advertised defaults.

### 11.2 A note on the Meters gating fix

§10.1's change — collecting key sources regardless of which view is open —
improves the **existing** key feature, not only Pitch. `[DETECTED KEY]` is built
from `keySources_`, so with collection gated on the Meters view the AI feed's
key block was equally starved whenever the user was on any other screen. That
was a live bug in a shipped feature; Pitch only made it visible.

### 11.3 formant_shift was built, measured, and REJECTED

The cheap implementation — resample each grain by a user-chosen ratio, sharing
the code path with `formant_mode = off` — does not do what the control claims.
Measured on a 900 Hz resonance with the pitch held fixed:

| shift | expected | measured |
|---|---|---|
| −12 st | 450 Hz | 300 Hz |
| −9 … −3 st | 535–757 Hz | **900 Hz (inert)** |
| 0 st | 900 Hz | 900 Hz |
| +3 st | 1070 Hz | **900 Hz (inert)** |
| +5 … +9 st | 1201–1514 Hz | 1500 Hz |
| +12 st | 1800 Hz | 2100 Hz |

Inert across half its range, quantised in ~600 Hz steps over the rest, and
non-monotonic at the bottom. Overlap-adding grains at the pitch period
reconstructs an envelope that barely follows the per-grain resampling, so the
control cannot work this way however the geometry is tuned. **It is not
shipped** — a knob that lies is worse than a missing one, and the spec's own
prescription (LPC or cepstral envelope: flatten, shift, re-apply warped) is a
different and larger piece of work.

**A measurement bug fell out of this and is worth recording.** The formant
tracker was a Goertzel, which is ill-conditioned off-bin and loses precision
badly over a 24,000-sample window. It had been reporting the source formant at
**749 Hz when it is exactly 900**, and P1's "PSOLA 905 vs resampler 1799" was
measured with it. Replaced with a direct DFT at a snapped bin: the source now
measures 900 exactly, PSOLA **900**, resampler **1804**. The P1 conclusion was
right; its numbers were not.

### 11.4 natural_vibrato: the first implementation was symmetric, not monotonic

Scaling "whatever vibrato survives correction" is the obvious reading and it is
wrong: correction removes the wobble along with the error, so 0% and 200% both
measured 27 ¢ of swing where 100% measured none. The control has to separate
the **note** from the **wobble** — correct the note, then add the singer's
oscillation back at the chosen amount. Measured after: **0% → 0 ¢, 100% → 27 ¢,
200% → 55 ¢**.

That split has a real consequence worth knowing: a *new* deviation is treated as
movement within the note until the slow pitch catches up (~140 ms), and only
then judged as the note being off. It stops the corrector chasing a scoop, at
the cost of engaging slightly later on a genuine drift.

---

## 12. One more regression the re-render caught

Re-rendering the character set after P5 showed **hard tune measuring worse than
the dry signal** — 15.7 ¢ mean deviation against 13.0 ¢, where before P5 it had
been 7.5 ¢.

Cause: §11.4's note/wobble split adds the singer's oscillation back at
`natural_vibrato`, and `correction_mode` never wrote that param — it did not
exist when the mode table was built at P3. So `hard` snapped the note and then
re-added the full wobble on top. Spec §4's table always specified it
(natural 100, balanced 100, tuned 40, **hard 0**); the implementation was simply
missing a column.

Fixed by completing the table. Hard tune returns to exactly its pre-P5 numbers:

| render | mean dev | median | within 5 ¢ |
|---|---|---|---|
| dry | 13.0 ¢ | 8.3 | 35.3% |
| hard tuned | **7.5 ¢** | **3.6** | **60.6%** |
| natural | 13.0 ¢ | 9.1 | 32.5% |

The lesson is worth keeping: **a mode is only as honest as its table is
complete.** Adding a character-bearing param without adding it to
`correction_mode` leaves the modes quietly wrong, and nothing but a re-measure
catches it — the suite was green throughout.

---

## 13. The Logic glitch: one shared shifter across both channels

### 13.1 What it was

`EedPitchProcessor` owned a single `PsolaEngine` and looped it over channels.
A `PsolaEngine` holds one delay ring and one write cursor, so channel 0 wrote
*n* samples and advanced the cursor, then channel 1 wrote its audio into the
next *n* slots of the same ring and read from a position *n* further on. The
ring ended up holding alternating blocks of L and R, and each channel was
rebuilt from the other's audio.

Measured, with **correction switched off entirely** — pure passthrough, where
the shifter only delays:

| | before | after |
|---|---|---|
| passthrough L vs R (identical input) | **0.762643** | **0.000000** |
| passthrough L vs delayed dry | **0.762643** | **0.000000** |

So the device garbled stereo *doing nothing at all*.

**Why nothing caught it:** the engine suites are mono, `pitch_render` builds one
engine per channel (the correct pattern, in the tool rather than the product),
and pluginval checks for NaNs and crashes, not for the audio being right.

### 13.2 Block-size coupling, found in the same pass

The corrector ran once per **block** with `dt = block duration`, so the retune
target was piecewise-constant per block — a step in the PSOLA ratio once per
buffer, i.e. a zipper on any gliding note. The block is now sliced at the
detector's own hop boundaries and the musical layer runs at that cadence.

| | before | after |
|---|---|---|
| fixed-512 vs fixed-128 / 256 / 1024 / 2048 | (not tested) | **0.00000, exactly** |
| fixed-512 vs random 16..2048 | 0.72593 | 0.19760 |

Exactness at a **constant** buffer size is the invariant that matters and it is
now bit-exact. A small residual remains under call-to-call size *variation*: no
time shift, ~1.5% of samples differing. Bounded by the test so a regression
still fails; documented rather than claimed fixed.

### 13.3 CPU: the worst case was a dropout, not a warning

`analyseHop` did its whole O(W × tauMax) difference function — about 576k
multiply-adds at instrument/bass — in whichever block contained the hop. At a
128-sample buffer the hop interval is ~128 input samples, so essentially every
block carried the full cost with nothing amortised.

Release build, 48 kHz, 128-sample buffer (2.667 ms period):

| voice_type | mean before | worst before | mean after | worst after |
|---|---|---|---|---|
| soprano | 6.2% | 92.7% | 6.1% | **16.0%** |
| alto_tenor | 7.9% | 90.8% | 7.7% | **9.6%** |
| low_male | 4.2% | 34.2% | 4.2% | **6.5%** |
| **instrument** | 20.1% | **132.3%** | 19.6% | **23.6%** |
| bass | 20.1% | 82.5% | 19.6% | **23.7%** |

The sweep now runs one contiguous slice per block, ahead of the audio. Two
things had to be right: doing it **per sample** instead fixed the worst case but
doubled the mean (a division each sample, and lost locality on the frame copy);
and the frame snapshot must happen **at hop boundaries only** — letting the
sweep capture "whenever it starts" made the captured frame depend on where block
boundaries fell and broke block-size exactness outright (fixed-128 vs fixed-512
差 0.80). Capturing at hops costs one hop of extra estimate lag (~2.7 ms), now
folded into `pitchLagFor`.

### 13.4 The suite-wide check, and two findings in other devices

The invariant is not "channels must match" — Stereoizer, Stereo Width, Auto Pan,
a ping-ponging Delay and Chorus spread all differ L/R on purpose. It is **"a
device must not produce a channel difference that no parameter asked for"**. So
every width, spread, pan, phase and image control is driven to neutral first,
and a device that still differs must declare an exemption with its reason.

19 of 22 pass at exactly 0.000000, including EchoJay Pitch. Three declare:

- **EchoJay Reverb** — legitimately decorrelated; a mono-in/stereo-out tail is
  the point of it.
- **EchoJay Chorus** — **non-deterministic**: identical input with `spread 0`
  (documented as "keeps both channels in step") gives 0.000002 on one run and
  0.166664 on the next, same binary, same input. Run-to-run variation with
  fixed input means **uninitialised state** — an LFO phase or a delay line not
  cleared in `prepare`. Listed, not fixed: not this session's device.
- **EchoJay Stereoizer** — ~0.0013 (−57 dBFS) with `width 100`, `haas_ms 0`,
  `mono_maker 0`. Unexplained, and well above the noise floor the other 21 sit
  at. Listed to be answered rather than tolerated.

The floor is 1e-5 (−100 dBFS), below the smoothing and denormal residue two
devices sit at (~−108 dB) and orders of magnitude below anything a control does.

Also fixed while in there: `PsolaEngine::process` now returns early if the
engine is unprepared (`mask_ == 0`), rather than indexing an empty ring. JUCE
orders `prepareToPlay` first so it was latent — but latent is a property of
today's callers.