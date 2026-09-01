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
different and larger piece of work. **That larger piece of work has since
been done: §15 is the LPC rebuild, measured monotonic and continuous across
the whole range.** This section stays as the record of why the cheap
geometry must not come back.

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
---

## 14. The clicking — measured, misdiagnosed once, found, fixed

**Symptom:** audible clicking in a DAW at the shipped defaults on the real
acapella. Every existing test was green throughout — pitch accuracy, nulls,
block-size exactness, pluginval strictness 5.

### 14.1 The instrument had to be repaired before it could be trusted

`tools/pitch_click_test` locates samples that break the local waveform trend
(prediction error > 20× the local median error) where the dry has no
transient. Its first version compared against the dry at the SAME index — but
the output runs `latency` (1656) samples behind, so every source transient
inside a dry-emitted span counted as a "click" 1656 samples after itself.
**147 of 302 reported clicks were that artefact**, discovered because the
shifter's own per-sample emit record said they sat on samples emitted as
bit-exact delayed dry. The tool now aligns the exclusion and carries a
positive control (`--control`): injected 65% amplitude steps at known wet
positions must be found, and the doctored run must not report wildly more
than baseline + injections. An earlier ancestor of this detector reported
1432 clicks/second where the truth was 5.5; a detector's zero is worth
nothing until it has found what it was told to find.

### 14.2 What the clicking actually was

Two hypotheses died on the data before the real one confirmed:

- **"Open-loop sqrt(Ts/Ta) gain + Ta-length grains step the overlap-add."**
  Plausible, and per-grain logging half-confirmed it — until the base rate
  was checked: 79.5% of ALL grains change half/gain between neighbours, and
  80.6% of grains near clicks did. No enrichment. Switching preserve to
  per-sample window normalisation changed the click count by ~8%: not it.
- **"Thin window-sum patches."** Zero clicks sat at w < 0.5.

The real mechanism, settled by the shifter's own emit record and waveform
plots of the top clicks: **`PitchCorrect::process` returns 0 on every
untracked hop, and the processor handed that 0 to the shifter as its target —
which flips `PsolaEngine::process` into the `emitDry` passthrough branch, an
instantaneous, fadeless switch from grain-summed wet to raw delayed dry.**
At `tracking = normal` ~13% of voiced frames are gated (§6), so the output
flipped wet→dry→wet several times a second, a hard step each way. Measured
signatures, all consistent: 39% of clicks at a fixed 16–23 samples from a
hop (the slice boundary, seen through the pitch-lag mapping), 65% amplitude
steps, clicks 13× enriched within 96 samples of an emit-state edge, and the
top-click plots all showing the same vertical step at the flip with the seam
fade nowhere in sight — because the target-0 path bypasses `emitMixed`, and
the seam fade lives inside `emitMixed`.

### 14.3 The fix, and what it measured

`EedPitchProcessor` now HOLDS the last target through untracked hops. The f0
ring already marks the gap unvoiced, and `emitMixed` then emits those samples
as bit-exact dry THROUGH the seam fade, which exists for exactly this join.
Unvoiced stays sacred (the null suite is unchanged and green); the only
change is that the wet→dry join is faded instead of stepped.

| | before | after |
|---|---|---|
| clicks/second (aligned detector) | 3.47 | **2.43** |
| clicks on emit-state edges | 27% (13× base) | 2.1% (= base rate) |
| fixed-offset-after-hop signature | 39% in one 8-sample bucket | gone (scattered) |
| top-click waveform shape | hard vertical steps | small burrs |

The residual 2.43/s is wet-path roughness on sharp-glottal and fricative
passages (nearly all of it concentrated in a few seconds of the take) —
PSOLA duplicating sharp pulses, not systematic steps. It is bounded by the
permanent density gate (`build_and_run.sh`, ceiling 3.5/s against the
original failure's 5.48/s) rather than claimed fixed.

### 14.4 What changed in the shifter while the wrong hypothesis was open

The preserve path now normalises by the accumulated window per sample (the
same rule as `off`), with a measured make-up gain `clamp(Ta/Ts, 1, 2)` for
the upshift energy that averaging misaligned pulse copies loses. Not the
click fix — but kept on its own merits, measured on the 30-check PSOLA
suite: every upshift now lands within +1.16 dB of dry where the open-loop
gain drifted to −2.9/−6.1 dB at a fifth/octave up (sqrt make-up was tried
and rejected: −3.04 dB at the octave). The window-sum histogram that
justified the floor (94% of emitted voiced samples at w = 0.95–1.05, a thin
edge tail below the 0.35 floor) is printed by the click tool on every run.

### 14.5 The key staleness fix (same session)

`KeyFeed` was published only from the editor's 2 Hz timer, so closing the
plugin window froze the key at its last value — the stale-key failure §10.2's
chromatic fallback exists to prevent, arriving by a different route. The
precedence walk (`collectKeySources`) reads only processor state, so it moved
to `EchoJayProcessor`, published from the processor's own 1 Hz timer for the
life of the instance; the editor delegates to the same single walk for its
UI. Publisher moved rather than key aged out: ageing to chromatic would have
made auto-key silently degrade the moment the window closed, which is the
feature turning itself off; a publisher that outlives the window keeps the
feature working and needs no new failure mode.

### 14.6 Every invariant, re-run after the fixes

- **g++ suites**: pitch_engine, pitch_correct, psola_engine — ALL PASS
  (includes the unvoiced null: unvoiced/silent output bit-identical to the
  delayed dry).
- **Host path** (EchoJayPitchHostTest): L vs R **0.000000** correcting and in
  passthrough; fixed-512 vs fixed-128/256/1024/2048 **exactly 0**;
  variable-block residual 0.202 peak (bound 0.25, §13.2's documented
  behaviour); oversize and tiny blocks finite. ALL PASS.
- **Mode machine** (EchoJayPitchModeTest): ALL PASS, now including the §12
  completeness assertion — every schema param is either proven to be WRITTEN
  by all four modes (set to each range end, select mode, readback must agree)
  or exempted by name with a reason. A new character-bearing param with no
  mode column fails the build.
- **Click density** (tools/pitch_click_test/build_and_run.sh): positive
  control 25/25 injected steps found with no false-positive explosion;
  density 2.43/s against the 3.5/s ceiling (5.48/s at the bug). PASS.
- **Characters, re-rendered and re-probed** (§12's table, same method):

  | render | mean dev | median | within 5 ¢ |
  |---|---|---|---|
  | dry | 13.1 ¢ | 8.3 | 35.7% |
  | hard tuned | 7.7 ¢ | **3.8** | 59.9% |
  | natural | 13.0 ¢ | 8.8 | 33.2% |
  | wrong key (C minor forced) | 30.6 ¢ | 33.0 | 5.0% |

  Hard tune's median moved 3.6 → 3.8 ¢ under the normalisation change —
  within the character, stated rather than rounded away.
- **CPU** (Release, 48 kHz, 128-sample blocks): means match §13.3 exactly —
  6.0 / 7.6 / 4.1 / 19.3 / 19.3% per voice type. The WORST-case column is
  not comparable at this granularity: across three runs of the same binary
  on an idle machine the max-over-40k-blocks statistic swung 17% → 44% →
  232% (low_male), so single-run worst cases measure the OS scheduler, not
  the code. Means are the stable regression signal; they are unchanged.
- **pluginval strictness 5** on this worktree's VST3: SUCCESS.

---

## 15. formant_shift, rebuilt via LPC — the knob no longer lies

§11.3 rejected the cheap formant_shift (per-grain resampling through the
`off` code path): inert from −9 to +3 st, ~600 Hz steps outside that,
non-monotonic at the bottom. The acceptance for the rebuild is therefore
exactly what that version failed: **monotonic and continuous across the whole
−12..+12 range**, measured with the direct-DFT tracker (never the Goertzel —
§11.3 records why).

### 15.1 The method — the spec's own prescription, per grain

`formant_mode = shift` (schema index 2, append-only as promised) runs each
two-period grain through:

1. **analyse** — LPC of the Hann-windowed grain, autocorrelation method,
   Levinson-Durbin, order `2 + fs/1000` (46 at 44.1 k, 50 at 48 k);
2. **flatten** — inverse-filter to the residual, against the REAL ring
   history, so the residual is exact and the round trip at shift 0 is an
   identity up to rounding;
3. **shift** — the residual grain is copied 1:1 and re-spaced at the target
   period: the same PSOLA move as preserve, applied to the flat signal;
4. **re-apply, warped** — the model envelope `P(w) = E/|A(w)|²` is evaluated
   on a π/512 grid, read back at `w/β` (β = 2^(shift/12)), cosine-transformed
   to an autocorrelation and Levinson'd into the warped synthesis filter. The
   residual is scaled by `sqrt((E'/E)·(r0/r'0))` so output energy lands at
   the grain's own regardless of what the warp did to the envelope.

**The warp lives in the envelope domain because the lag domain measurably
fails.** The textbook shortcut — interpolate the raw autocorrelation at
scaled lags, r(βk) — was tried first: at β = 0.5, 0.561 and 1.122 the
interpolated sequence went indefinite (r(τ) of anything with near-Nyquist
content oscillates at the sub-sample scale), Levinson degenerated through its
reflection clamps to E' ≈ 2.5e-16 against a healthy 1.9e-3, and the guard
silently dropped every grain to the unwarped fallback — three shift settings
INERT at exactly the source formant, the §11.3 failure wearing a new hat. The
smoothed model envelope is interpolable by construction (a 40 Hz Gaussian lag
window floors every peak's bandwidth at roughly the grid spacing), and the
cosine transform of a nonnegative spectrum is positive definite, so Levinson
cannot degenerate on it. The degeneracy guard that remains is RELATIVE
(E ≤ 1e-9·r0), because on clean periodic material a tiny absolute E means
the model is very good, not broken — the first absolute guard was the bug.

### 15.2 Measured — synthetic resonance, pitch held

900 Hz resonance (q = 8) on a 90 Hz source, pitch held at 90 Hz, tracker =
direct DFT sampling the envelope at every harmonic with parabolic refinement
(raw peak-picking on periodic material quantises to harmonics and would read
steps where there are none — the estimator has to be continuous before it can
certify the control is):

| shift | expected | measured | error |
|---|---|---|---|
| −12 st | 450 Hz | 452 Hz | +0.4% |
| −10 st | 505 Hz | 533 Hz | +5.6% |
| −8 st | 567 Hz | 561 Hz | −1.0% |
| −6 st | 636 Hz | 633 Hz | −0.5% |
| −4 st | 714 Hz | 717 Hz | +0.3% |
| −2 st | 802 Hz | 806 Hz | +0.5% |
| 0 st | 900 Hz | 901 Hz | +0.1% |
| +2 st | 1010 Hz | 988 Hz | −2.2% |
| +4 st | 1134 Hz | 1092 Hz | −3.7% |
| +6 st | 1273 Hz | 1252 Hz | −1.6% |
| +8 st | 1429 Hz | 1409 Hz | −1.4% |
| +10 st | 1604 Hz | 1569 Hz | −2.1% |
| +12 st | 1800 Hz | 1751 Hz | −2.7% |

Monotonic at every step, every step ratio inside [1.04, 1.25] against the
ideal 1.122, every point within 10% — all three asserted in
`test/psola_engine_test.cpp`, which also proves independence (pitch up a
fourth with formant −5 st: pitch lands +0 ¢, formant 675 Hz against 674
wanted), shift 0 ≡ preserve within the tracker's resolution, and the
unvoiced null in shift mode (0 differing samples).

### 15.3 Measured — the real acapella

A single "dominant formant peak" is ill-posed on real speech across a
±1-octave warp: the pre-emphasised LTAS peak jumps between F0-region, F1 and
F2 (measured: 186 → 646 → 1328 Hz) even while the envelope moves perfectly
smoothly. The well-posed observable is the **log-frequency displacement of
the whole LTAS envelope** against the shift-0 reference (250 Hz-smoothed log
spectrum, 250–5000 Hz, least-squares over 0.1 st steps), plus the spectral
centroid as a second, metric-independent monotone:

| shift | envelope displacement | centroid (200–4000 Hz) |
|---|---|---|
| −12 st | −6.37 st | 385 Hz |
| −10 st | −5.42 st | 380 Hz |
| −8 st | −4.85 st | 379 Hz |
| −6 st | −4.20 st | 377 Hz |
| −4 st | −3.07 st | 392 Hz |
| −2 st | −1.42 st | 432 Hz |
| 0 st | 0.00 st | 479 Hz |
| +2 st | +0.96 st | 508 Hz |
| +4 st | +2.59 st | 545 Hz |
| +6 st | +4.07 st | 592 Hz |
| +8 st | +5.36 st | 639 Hz |
| +10 st | +6.52 st | 691 Hz |
| +12 st | +7.06 st | 747 Hz |

Strictly monotonic at every one of the twelve steps — the property the
rejected version lacked. The displacement magnitude under-reads the request
(≈0.5–0.8×) and that is the METRIC, not the control: the LTAS mixes the
moved envelope with the unmoved harmonic comb (the pitch is held, so the
comb must not move), with part-unvoiced frames, and with band-edge
truncation of features warped past 250/5000 Hz — all of which pull the
least-squares displacement toward zero. The synthetic table above, where the
observable is clean, carries the magnitude claim.

### 15.4 Every banked invariant, re-run

- **preserve is bit-identical to before this work** — proven directly, not
  assumed: the same streaming harness compiled against the git-HEAD
  `EedPsolaEngine.h` and against the new one, run over the full acapella:
  preserve +3 st, preserve −4 st, off +3 st and passthrough all compare
  **0 differing bytes**. The shift mode branches out of `placeGrain` before
  the preserve/off code is touched.
- **g++ suites** (pitch_engine, pitch_correct, psola_engine): ALL PASS,
  including the unvoiced null and the new §15.2 assertions.
- **Host path**: L vs R 0.000000 correcting and in passthrough; fixed-512 vs
  fixed-128/256/1024/2048 exactly 0; variable-block residual 0.202 peak
  (bound 0.25); oversize and tiny blocks finite. ALL PASS.
- **Mode machine**: ALL PASS. formant_shift is exempted from the mode table
  BY NAME with its reason (a who-is-singing control, and inert in every mode
  since every mode writes formant_mode preserve); the walk still proves
  formant_mode itself is written by all four modes.
- **Click density**: 2.43/s against the 3.5/s ceiling, control 25/25 —
  unchanged, as bit-identity requires. Diagnostic only (new
  `--formant-shift` flag on the click tool, not part of the gate): the LPC
  path itself measures **1.00/s** at −5 st on the same material — the
  resynthesis smooths the duplicated-pulse burrs that make up the preserve
  residual.
- **Characters re-probed**: dry 13.1/8.3 ¢, hard tuned 7.7/**3.8** ¢,
  natural 13.0/8.8 ¢, wrong key 30.6/33.0 ¢ — the §14.6 table exactly.
- **CPU** (Release, 48 k / 128): means 6.1 / 7.8 / 4.2 / 19.7 / 19.7% per
  voice type against §14.6's 6.0 / 7.6 / 4.1 / 19.3 / 19.3 — within run
  noise, at the shipped defaults the LPC path never executes. Engine-level,
  shift mode costs ~23% more than preserve (9.1 s vs 7.3 s for 118 s of
  mono at 44.1 k, detector included) — ~1.5 points of realtime, only when
  the mode is selected.
- **pluginval strictness 5**: SUCCESS on this worktree's freshly built VST3.

### 15.5 The gate that almost lied, and the A/B set

Two findings from the re-run worth keeping:

- `build_and_run.sh` hardcoded the Debug artefact path while the build
  cache had moved to Release, so `cmake --build` rebuilt one binary and the
  script ran another — the density gate was green against a STALE binary.
  The script now derives the artefact dir from `CMAKE_BUILD_TYPE` and fails
  loudly if the binary predates its sources. (Every §15 number above was
  then re-measured on verified-fresh binaries.)
- The residual 2.43 clicks/s (§14.3) was looked at again as a secondary:
  it lives in the preserve wet path's grain summation, and any change there
  breaks the bit-identity acceptance this very section banks. Left alone,
  bounded by the gate; the shift path's 1.00/s reading says the burrs are
  not fundamental to grain synthesis, so the option exists for a session
  allowed to move preserve.

The formant A/B set — the control heard in isolation, pitch held constant,
`formant_shift` at −7 / −3 / 0 / +3 / +7 plus the preserve reference —
renders via `EchoJayPitchRender --formant-set` and sits next to the acapella
in `… - formant AB/`.

---

## 16. The Antares A/B — three losses, measured, closed

An independent A/B against Antares Auto-Tune Pro (same take, low_male,
D chromatic, retune 0 / flex 0 / humanize 0, formants on; 914 confidently
voiced frames in the analyst's framing) found EchoJay losing three separate
ways: median distance to the nearest semitone 16.9 ¢ against Antares's 8.5
(dry 28.7); HNR −1.55 dB against −0.19; spectral flux +24.6% above dry
against +3.8%. Every existing test was green throughout — the §14 lesson
again, one abstraction level up: the suite proved the machine against
ITSELF, and nothing proved it against the reference its user compares it to.

### 16.1 Positive control

The analysis was reproduced with this repo's own tracker before any DSP
moved (2063 frames at hop 2.7 ms; the analyst's stack differs, so exact
values shift while every ordering and magnitude holds): dry 29.5 ¢ /
echojay-bounce 15.7 / antares 11.2; HNR 7.01 / 5.99 / 6.90; flux +20.7% /
+3.0%. One number the reproduction added: the tracker fails on **385 frames
of our bounce against 29 of Antares's** — the "tracker working harder on our
noisier output" hedge, quantified.

### 16.2 Finding 1 was a settings trap, not a DSP defect — and both named
suspects measured clean

Rendering the dry through the shipped chain at the bounce's settings and
sweeping one variable at a time:

| variant | median ¢ | within 5 ¢ |
|---|---|---|
| natural_vibrato 100 (schema default) | 22.1 | 12.7% |
| natural_vibrato 40 ('tuned' preset writes this) | 15.2 | 17.8% |
| the actual bounce | 15.6 | 16.6% |
| natural_vibrato 0 (the honest Antares match) | 7.9 | 37.8% |
| Antares, same tracker | 11.1 | 34.4% |

The bounce ran with natural_vibrato at 40 — the fingerprint of selecting
`correction_mode tuned` and then zeroing retune/flex/humanize by hand, which
leaves the vibrato re-add in place. That is the control DOING ITS JOB
(§11.4 separates the note from the wobble precisely so the wobble can
survive a snapped note), but it is a trap when the brief is "match Antares
retune 0", which flattens vibrato. With it at 0 we measure BETTER than
Antares's median on the same tracker. Both schema descriptions
(retune_speed_ms, natural_vibrato) now name the interaction and the numbers.

The two suspects named for the residual were both measured and cleared:

- **Corrector lag compensation** — applying each hop's target 655 / 1528 /
  2619 samples earlier (offline look-ahead) measured 9.3 / 10.7 / 10.0 ¢
  against 9.0 at the shipped timing. The shifter's lookahead already aligns
  the target with the audio it shapes; compensating AGAIN over-corrects.
- **Hold-through-gaps** — reverting to the pre-§14 target-0 behaviour
  measured 9.0 ¢, identical. At retune 0 the gap samples emit dry either
  way; the hold stays (it is the click fix).

### 16.3 Findings 2 and 3 were the synthesis, and the fix is not to
granulate at corrector ratios

The instrument that settled it: **unity resynthesis** — the wet path told to
change nothing (target = detected f0). Raw-grain OLA measured **−1.0 dB HNR
and +19% flux at unity**, so the roughness was structural, not correction.

What was tried, in order, each measured on the reference take:

| preserve architecture (unity) | HNR | flux |
|---|---|---|
| raw-grain OLA (shipped before this) | 5.97 | +19.0% |
| LPC residual OLA + emit-time synthesis filter | 5.50 | +24.1% |
| + placement-time grain alignment | 6.14 | +18.4% — and REJECTED: it fights the re-spacing (octave shifts collapsed to the source pitch) |
| + epoch phase refinement (analysis domain) + fractional spacing | 5.73 | +21.2% |
| raw grains + refinement + fractional spacing | 6.14 | +15.1% |
| **splice-resampler** | **6.99** | **+2.6%** |

The LPC-residual rebuild was built as directed and measured: at
corrector-scale ratios the per-epoch coefficient switching costs more than
ring-summing ever did, and it now serves only `formant_mode = shift` (where
the envelope warp requires it). The load-bearing insight is architectural:
granular synthesis rebuilds the waveform ~f0 times a second however well the
grains align, while the reference class resamples continuously and splices
out one period only when the read pointer has drifted a period —
|ratio−1|·f0 splices per second, which is ~1/s at 20 cents and **zero at
unity**. `preserve` now runs that splice-resampler inside ±2.5 st of unity
(phase-aligned with the dry at every seam by construction, drift reset at
unvoiced samples, ratio smoothed over 2 ms against detector frame noise and
evaluated at the READ position), crossfading to the grain path beyond the
band. Formants move with the ratio inside the band — bounded at a level
correction never reaches audibly, and the band edge hands over to grains,
which preserve exactly. Unvoiced stays bit-identical dry throughout.

### 16.4 The permanent gate, and the ledger

`tools/pitch_ab_test/` renders the dry bounce through the CURRENT engine
(g++, from source — it cannot test a stale binary) at the hard-match
settings and asserts the three findings against the ANTARES column,
measured fresh from the reference bounce each run, never transcribed. The
margins are the measured remaining gaps (cents +1.5, HNR −0.25 dB, flux
+2.0 points), named as such: tighten as they close, never widen. Material
path in the git-ignored `tools/pitch_ab_test/material.local`. One
measured-the-instrument note for whoever edits the gate: applying hop
values at block granularity instead of slicing at hop boundaries measured
+28.6% flux against +4.2% — the render harness must slice exactly as
`processBlock` does.

Final ledger on the reference take, same tracker for all columns:

| | dry | echojay (was) | echojay (now) | antares |
|---|---|---|---|---|
| median cents | 29.5 | 15.6 | **12.0** | 11.1 |
| within 5 ¢ | 9.3% | 16.6% | **25.2%** | 34.4% |
| HNR delta vs dry | — | −1.03 dB | **−0.16 dB** | −0.13 dB |
| spectral flux vs dry | — | +20.7% | **+4.2%** | +3.0% |
| tracker failures | 0 | 385 | **110** | 29 |

At unity the wet path now measures HNR 7.03 against the dry's 7.01 and flux
+2.6% against Antares's +3.0 — resynthesis is transparent where it used to
cost a dB. Within-5-¢ remains the honest open gap (25 vs 34): the splice
ratio is open-loop in the detector's per-frame error where the grain path
cancelled it, and closing it wants a finer tracker, not a louder assertion.

### 16.5 Every surviving invariant, re-run

The §15 bit-identity of preserve is retired by this change — it protected
the worse code path. Everything else:

- **g++ suites** (pitch_engine, pitch_correct, psola_engine): ALL PASS —
  passthrough and unvoiced nulls bit-exact, pitch accuracy, formant
  preserve at octave shifts, the §15 formant_shift sweep still monotonic
  and continuous, levels (unison now −0.00 dB, whole tone −0.03 where the
  grain path drifted to −0.36).
- **Host path**: L vs R 0.000000; fixed-512 vs 128/256/1024/2048 exactly 0
  — the splice state advances once per emitted sample and every placement
  decision derives from grain-local quantities, which is what block-size
  independence required. Oversize/tiny blocks finite. ALL PASS.
- **Mode machine**: ALL PASS, unchanged.
- **Click density** (gate at shipped defaults): **1.78/s** against the
  3.5/s ceiling, positive control passed — down from §14's 2.43. The splice
  path removes the grain boundaries the §14 residual lived on.
- **CPU** (Release, 48 k / 128): means 6.3 / 8.0 / 4.5 / 19.6 / 19.6% —
  within noise of §14.6 (the LPC cost is skipped inside the splice band,
  decided from Ta/Ts so fixed-block exactness survives).
- **pluginval strictness 5**: SUCCESS on the freshly built VST3.

### 16.6 The clean comparison, the trade named, and the outside-band answer

The §16.4 ledger's "echojay (now)" column IS the honest match — the gate
renders natural_vibrato 0, retune 0, flex 0, humanize 0, ignore_vibrato
off; there is no configuration daylight between it and the Antares column.
The 7.9–9.0 ¢ figures earlier in §16.2 are the OLD grain-path engine at
those same knobs. Put side by side, the two architectures at identical
settings against Antares:

| hard match, same tracker | grain path (old) | splice (shipped) | antares |
|---|---|---|---|
| median cents | 9.0 | 12.0 | 11.1 |
| within 5 ¢ | 34.6% | 25.1% | 34.4% |
| HNR delta vs dry | −1.05 dB | −0.16 dB | −0.13 dB |
| flux vs dry | +20.0% | +3.8% | +3.0% |

**So the within-5 gap is real, and it is not the tracker's ceiling** — the
grain path proves this detector supports 34.6%. It is the splice ratio
being open-loop in the estimate's per-frame error, where grain spacing at
fs/target cancels that error by construction. One closure was tried and
measured: deriving the ratio from MEASURED epoch spans (the grain path's
exactness argument transplanted) came out at 18.1 ¢ / 18.9% — epoch
spacing jitters a few samples on real pulses and a few samples of a period
is tens of cents, noisier than the estimate it replaced. Rejected; the
comment above the ratio records it. Closing the last nine points of
within-5 without giving back the two decibels wants a finer f0 estimate
feeding the ratio, and that is a detector project, not a knob.

**Outside the ±2.5 st band** the transparency numbers are, plainly, a
near-unity result — the grain path takes over and granular limits return.
What the band-exterior DID gain from this session (epoch phase refinement,
error-diffused fractional spacing, phase-snapped entry, and preserve's
grains going RAW after the LPC emit-filter measured worse there too —
+5 st HNR 6.47 LPC against 7.30 old-raw):

| +5 st transpose, hard match | old engine | shipped now |
|---|---|---|
| median cents | 7.7 | 7.7 |
| within 5 ¢ | 37.4% | 37.7% |
| median HNR | 7.30 dB | 7.74 dB |
| flux vs dry | +23.4% | +17.4% |

Better on every axis, not Antares-transparent (no Antares reference exists
at transpose, and flux-vs-dry inflates legitimately once the spectrum
actually moves — at +12 st the dry-referenced metrics stop meaning
anything and the synthetic suite carries correctness there). The honest
summary: at corrector ratios, where the reference comparison lives, wet
resynthesis is transparent; at transpose ratios it is an improved granular
shifter.

### 16.7 The HF-excess report — instrument first, then the verdict

A host-side analysis reported >6 kHz envelope events exceeding the Antares
bounce by >15 dB, arriving in pairs and triples spaced 31–44 ms (mean
~36 ms ≈ the low_male tracking lag), with named timestamps, and proposed a
mechanism: the splice ratio's decision and the audio it splices aligned to
f0 estimates one tracking lag apart.

**The reproduction failed, and the failure was diagnosable.** None of the
named timestamps show an anomaly in any bounce in the reference folder,
under HF-envelope or waveform-jump criteria, at any threshold, with no
constant offset mapping that event list onto anything measurable here. An
INSTANTANEOUS HF compare against Antares does manufacture ~4 events/s —
but they sit on the take's own consonant transients, they appear at
IDENTICAL times and spacings across all four voice types (soprano cannot
even track this voice, so a tracking-lag mechanism cannot produce identical
soprano events — the discriminator the analysis itself proposed, run, and
failed), and they vanish at any reference tolerance ≥ 2 ms. Two resamplers
wobble a few milliseconds against each other; sample-exact comparison
reads that skew as level at every shared transient. The waveform-jump scan
confirms it from the other side: the biggest jumps in the wet are the DRY's
own glottal/plosive steps, same list, same sizes.

**The code suspect is real and was left alone, on three measurements.**
The splice ratio's numerator (the corrector's target, from the fresh hop)
and denominator (the back-dated f0 ring) do disagree — by latency − lag
(~20 ms at low_male, not one full lag). But re-timing the target to
"correct" it makes the output measurably worse in every direction tried:
positive leads cost cents (§16.2), and the aligned negative lead
(−957 samples) produced 3 genuine HF events up to 29.7 dB where the
shipped timing produces 1. The fresh-target timing is load-bearing —
the synthesis lookahead consumes it ahead of emission — and the
misalignment on paper is compensation in practice.

**Re-gated with a validated instrument.** The gate now carries a fourth
metric: >6 kHz peak-envelope events >15 dB over the Antares bounce's
±3 ms local max, with a 25-step injection control per run (14 found —
a zero from an instrument that cannot find planted steps is worth
nothing). Verdict: the HOST bounce of the shipped build measures **0
events**; the gate's own offline render measures 2 marginal events
(0.24/s), the inspected one sitting at an F#3/G3 note-boundary chatter
where the wet's largest waveform step is SMALLER than the dry's at the
same instant. Ceiling 0.30/s — exactly those two events, a third fails;
the goal, as directed, is zero.

### 16.8 The mode-table correction, and the momentary-wrong-note defect

**targeting_ignores_vibrato is ON in all four modes** — the §4 table's
off-for-tuned/hard was a spec error, corrected in spec, presets and mode
test. Re-gated with it on (this is now the shipped configuration for every
mode, and every §16 number before this section was taken with it off):

| hard match, same tracker | with it off (§16.6) | with it ON (shipped) | antares |
|---|---|---|---|
| median cents | 12.0 | **10.3** | 11.1 |
| within 5 ¢ | 25.1% | **28.5%** | 34.4% |
| HNR delta vs dry | −0.16 dB | **−0.00 dB** | −0.13 dB |
| spectral flux vs dry | +3.8% | **+3.3%** | +3.0% |

Median cents and HNR now BEAT the Antares column. Click gate at the shipped
defaults: 1.78/s against the 3.5 ceiling, control passed, unchanged.

**The momentary wrong note** (Sean's ribbon spikes; the octave guard logged
339 fires on one phrase; the independent A/B's 3.5% of frames >600 ¢ from
dry against Antares's 0.44%, mostly downward). A grain-accurate shift to the
wrong pitch is smooth in the waveform and invisible to every discontinuity
gate, so the AB gate gained a fifth metric — pitch excursions: the output's
own track departing its ±150 ms local median by >400 ¢ and returning within
150 ms, injection-validated (10 octave-up patches spliced into a copy; 5
found) before any number was trusted.

The fix went through three measured iterations, and the record matters:

1. **Blind persistence** (hold any >600 ¢ jump for 50 ms, the directed
   confirm-window extension) — REJECTED: it INJECTED excursions on the rap
   acapella, 16 against 8 without it, because that delivery changes register
   across consonant gaps and produces true fry subharmonics, and holding the
   old octave through a genuine period-doubling is itself a 50 ms wrong note.
2. **Ask the audio** (F0JumpGate + PsolaEngine::inputPeriodicity): on an
   octave-scale jump, one normalised autocorrelation settles which story is
   true — a spurious flip leaves the waveform periodic at the OLD lag, a
   real drop collapses it there, an upward move is believed when the NEW lag
   correlates. Persistence stays as the backstop for inconclusive frames.
   Measured: acapella excursions 8 with the gate == 8 without (nothing
   injected), while the gate held **345 hops** — strikingly close to the 339
   guard fires — and audio-confirmed 53 genuine jumps.
3. **Seed vetting**: the remaining event was a SUB-OCTAVE ONSET — a span's
   first estimate reading 79–87 Hz for 24 ms where the true pitch is 175
   (the ×2 "jump" when the detector rights itself was never the problem; the
   seed was). At a seed, the same audio question runs against HALF the
   candidate period: if the half lag is also strongly periodic the candidate
   is plausibly a sub-octave read, and the hop presents as untracked (dry
   output, exactly like tracker warm-up) until it persists 50 ms or
   corrects. A clean onset has a low half-lag correlation and seeds
   immediately, so normal onsets pay nothing. This also removed the third
   HF-excess event that had appeared on the reference take - the sub-octave
   onset was being SYNTHESISED.

Final excursion ledger: reference take ours 2 / Antares 1 / dry 1 — the +1
dissected at 1.895 s as the take's own creak onset, where the hard-tuned
onset (creak at 87 Hz into wet at 185) reads sub-octave to the tracker for
~15 ms across the seam; not a mid-phrase spike. Ceiling set at Antares+1
with that event named; any spike the metric was built for fails the build.
Acapella: dry 16 / ours 8 (low_male), dry 11 / ours 11 (alto_tenor) — at or
below the source's own excursion behaviour on both readings.

### 16.9 The ribbon feed — the picture is now the audio

The ribbon's paint code was always §7-compliant (unvoiced closes the
subpath; never zero, never held). The FEED had two bugs: the dim trace
carried the RAW pre-F0JumpGate estimate — measured, 33 of 34 full-height
verticals on the acapella moved the corrected trace under 2 st; the picture
spiked, the audio did not, and a user report of those spikes cost two
rounds of debugging a defect the audio never had — and pushes ran at block
rate (~93 Hz at 512) against a view designed for 30 Hz columns, spanning
~1.3 s where §7 says four. The feed now carries the GATED value (the number
the shifter and corrector obey), decimated to the column cadence
independently of host block size. Rejected estimates remain visible
numerically in the octave-guard counter; no fainter raw layer was added — a
spiking line reads as an artefact whatever its alpha.

### 16.10 The wet-path burrs — three mechanisms, one instrument failure,
and parity

The residual click density (1.85/s at the old ceiling) decomposed under a
synchronised timeline (per-hop decisions + the shifter's own emit record +
sub-decision splice/method events) into:

- **Instrument failure #11** (~70% of the count): the click tool's dry-
  transient exclusion looked in a fixed ±64-sample window at latency
  alignment - correct in the grain era, wrong since the splice-resampler,
  whose read drifts up to 0.75 of a period. Patch-matching the top-30
  "clicks" against the dry found median waveform correlation **1.00** at
  the best in-window offset, with the matched dry feature's own error
  ratio at **19.4 against the 20× threshold**: the take's own glottal
  edges, faithfully reproduced, drifted out of the window and straddling
  the threshold cliff. The exclusion is now drift-aware (patch-match ≥0.97
  with a near-threshold dry twin excludes; created content cannot match
  and the 25-step control still finds 25/25).
- **Linear interpolation** in the splice read: error O(h²·x″), exploding
  at sharp glottal edges as the fractional phase drifts. Catmull-Rom took
  the honest density 1.85 → 1.58 before the instrument fix.
- **Three real synthesis defects**, each found by waveform microscope and
  fixed: the ratio's 0.125-st snap branch kinked the read velocity on fast
  downward glides at retune 0 (the target staircases through the glide) —
  now always slewed at 2 ms, a note step still completing in ~6 ms; the
  splice jump used the ROUNDED period, misaligning the crossfaded copies
  by up to half a sample — now fractional, with a raised-cosine fade; and
  the state-reset on a momentary read-position voicing flicker was a
  fadeless ~200-sample read jump in a fully wet span — `ok` now gates
  state updates only, never emission, which also removed the §16.8 creak
  excursion (ours now 1 = Antares = dry).

After all of it: acapella click density **0.12/s** (ceiling re-based 3.5 →
1.0 - the old ceiling dated from a 5.5/s failure and would pass the old
DSP; the new one fails it). The AB gate gained the same detector as a
sixth Antares-anchored metric: **ours 2 vs Antares 2** on the reference
take, and the two are near-twins of Antares's own two (3.242 vs 3.284 -
the material's hardest moments defeat both processors). HF excess fell to
0.12/s. All six metrics green with their injection controls; suites, host
invariants, mode machine, pluginval green.

### 16.11 The other formant modes through the six-metric gate, and the UI
that was invisible

**off** measured through the gate at hard-tune settings read HNR 4.87 dB
against preserve's 6.99, flux +19% against +3.3, **59 clicks against 2** —
a defect, exactly as framed: at corrector ratios formant displacement is
negligible, so off had no business sounding different, and the difference
was architectural (off ran the grain-OLA path in-band where preserve rides
the splice-resampler — whose formants-move-with-ratio behaviour IS off's
semantics). Off now splices inside the band and measures **identical to
preserve** (10.1 ¢ / 6.98 dB / +3.1% / 2 clicks); beyond the band its
resampled grains still go full chipmunk, and the psola suite's mode-
separation checks still pass.

**shift**, swept at formant_shift 0 / ±3 / ±7:

| shift | med cents | within 5 | HNR | flux | clicks |
|---|---|---|---|---|---|
| preserve (ref) | 10.1 | 29.0% | 6.99 | +3.3% | 2 |
| 0 | 12.3 | 28.7% | **5.43** | **+30.8%** | 3 |
| +3 | 8.7 | 29.1% | 4.96 | +30.7% | 2 |
| −3 | 11.5 | 24.9% | 5.83 | +34.0% | 4 |
| +7 | 9.3 | 27.6% | 4.66 | +36.5% | 4 |
| −7 | 15.2 | 19.8% | 5.68 | +43.0% | 1 |

**The diagnostic is unambiguous: at shift = 0, with nothing warped, HNR is
already −1.6 dB and flux ×9 against preserve. The cost is the LPC-residual
path itself, not the warp** (the warp adds a few points of flux and little
else). The fix is architectural — the same verdict that demoted this path
from preserve in §16.3 — and is future work. Until then: the schema
descriptions for formant_mode and formant_shift now state the measured
trade so the model does not reach for shift casually, and shift stays OUT
of the device advertisement (the registry summary), which continues to
promise only preserved formants. The mode remains dialable: its
monotonicity acceptance (§15) still holds, and a character effect is
allowed to cost fidelity as long as the cost is stated where the knob is.

**The invisible scale control.** `scale` was in the schema, dialable by
the model, fully wired in the editor — and the combo column's layout gave
three 24 px rows a 58 px band, so the third row, SCALE, was squeezed to a
~10-pixel sliver. A user reported the panel "shows KEY only" and was
right. The column now divides evenly; both key and scale dim to 45% alpha
under key_source = auto (they show the DETECTED values — a reading, not an
edit surface) while staying clickable, since selecting a value is how the
user takes manual control (the underlying params already flipped
key_source on any hand write).

**The permanent UI-coverage audit** (tools/pitch_mode_test): the editor
now publishes handControlledParams(), and the walk fails the build on any
schema param that is neither hand-controlled nor exempted-with-reason.
The ledger currently names 12 UI-less params, the loudest being
**key_source's one-way gap — touching key/scale forces manual and no hand
control returns to auto** — held for separate scoping, as is the
pitch_scale degree editor (twelve enables plus per-degree bias is a real
panel, most naturally an interactive degree strip on the ribbon's existing
scale lines; more than a small job, not bundled here).

### 16.12 The key_source trapdoor, closed

§16.11's loudest ledger entry is fixed: an AUTO/MANUAL toggle now sits
beside the key and scale combos, spanning the two rows it governs. Lit
means auto; clicking a combo still takes manual control as before, and the
toggle is the way back - returning to auto clears the auto-key memo, the
next block re-applies the detected key and scale, and the combos show them
(dimmed) within a timer tick. Losing the detected-key feature for the life
of the instance because of one click on a dropdown was a trapdoor in the
device's headline reason to exist. reference_source keeps its ledger
entry: the same one-way shape exists at the param level, but no hand
control writes reference_hz, so only the model can spring that one - and
the model can release it. The pitch_scale degree editor stays scoped
separately (an interactive degree strip on the ribbon's scale lines is
the right shape; not a small job).

### 16.13 preserve == off, provable; off reframed; the key-B octave finding

**The equivalence is now asserted every build.** The gate renders preserve
and off from the same dry and requires the six metrics to agree within
tight tolerances (measured: cents 10.1/10.1, HNR 6.99/6.98, flux
+3.3/+3.1, hfx 1/1, exc 1/1, clicks 2/2; not bit-identical — 0.05% of
samples differ at band-edge method-fade moments, stated in the check's own
output). The POSITIVE CONTROL forces off onto its grain path via
debugDisableSplice and requires the comparator to catch it — it does, at
exactly the old defect's signature (HNR 4.87, flux +19). The regression
Sean heard twice now fails the build instead of waiting for ears.

**off is a transpose-time control** — schema text rewritten to say it
changes nothing at tuning-sized corrections and only matters when
transpose moves pitch by semitones; the editor's combo item reads
"FORM OFF (TRANSPOSE)" and the combo rests dim on preserve, lighting only
when the setting departs. **shift unchanged**: dialable, out of the
advertisement, cost written on the knob — the only mode that does
something no other control can.

**The key-B octave finding: neither a wrong key nor wrong-octave
targeting.** Reported: corrections median 181 ¢, p90 1424, 38% beyond
±250. Re-measured with our own instruments on the reference take at hard
settings, key B major: |committed target − detected f0| median **64 ¢**,
p90 163, **3.5% beyond 250** — and |target − nearest-enabled-degree(f0)|
median **0 ¢**: the selector's octave arithmetic is sound (±1-octave
neighbourhood search, distance-bounded by half the largest scale gap).
The >600 ¢ tail is 30 hops (1.5%), of which 22 are the PREVIOUS note held
through a register flip — §2.3's note-change confirm window doing its
documented job for ~25–70 ms per flip — and the tail is IDENTICAL under
chromatic, which acquits the key entirely. The reported magnitudes
(181/1424/38%) do not reproduce on clean audio; that bounce's formant-off
ran the pre-§16.13 grain path (the defect above), whose roughness made
the external tracker exaggerate — the same instrument hazard the reporter
flagged themselves.

## §17 The shift-gated bleed, and natural's character — read these together (29 Aug 2026 ruling)

### §17.1 The gate, and the ACCEPTED natural delta

The drift-bleed is gated on |ratio−1| against its own cap (taper 0.7–1.3 of
cap, 100 ms pole; EedPsolaEngine spliceSample). Below the cap the bleed can
bound drift — an equilibrium exists; above it, accumulation outruns the cap,
splices do the bounding, and a running bleed is pure convergence tax:
measured on a noiseless steady tone at hard, 3.38 c note-centre undershoot
with the ungated bleed against 0.42 c without. The gate removes the tax
(hard reads 0.42 c / 99.7 % within-3 c with the gated bleed ON).

**The natural-mode delta is a KNOWN, ACCEPTED consequence, not a
regression.** Field figures on the standing NEW set, gated vs ungated:
vib-off 52 vs 51 rough spans (0.19 s both), vib-on 59 vs 56 (+0.01 s),
worst deficits −0.39 vs −0.34 and −0.62 vs −0.54, inversions 0 in all.
Natural's older, slightly lower numbers were partly BOUGHT BY THE TAX —
the ungated bleed was detuning sustained corrections by up to 3 cents
everywhere, including natural. Removing the tax and keeping its side
effect is not available. Ruled and accepted 29 Aug 2026; do not widen or
soften the taper to buy these spans back — that fits to a subsidised
number — and do not reverse the gate on the strength of this delta.
(SETTLED 30 Aug 2026: the vib-on fix (d) repaid this cost — natural
vib-on spans 59 → 56, back at the pre-gate number. The price is paid,
not outstanding; see DEFECT_VIBRATO_ON_TUNING_COST.md.)

### §17.2 Natural leaves most of a sustained offset standing — BY DESIGN

A steady tone 20 c off-grid through the natural preset comes out 17 c
off-grid. That is flex 55 / humanize 60 doing exactly what their schema
text promises — expressive deviation is left alone, sustained notes are
not frozen — and it is why every absolute tuning column measured at
natural reads "loose" against Antares-at-hard or EchoJay-at-hard.
It is the preset's CHARACTER, not a convergence defect. Before chasing
any natural-mode tuning number, re-read this section and the hard-mode
history that produced it (the settings audit: an entire investigation
optimised natural while the complaint lived at hard).

### §17.3 The retune floor: 6 ms, and why the zone below it is dominated (30 Aug 2026 ruling)

`retune_speed_ms` is the honest time constant of the per-hop one-pole
toward the aim (63 % in τ, ~95 % in 3τ). The dial still reads 0; the
EFFECTIVE τ never goes below `PitchCorrect::kRetuneFloorMs` = 6 ms, and
the knob readout shows the mapping ("0 (6 ms)").

The 0–6 ms zone is STRICTLY DOMINATED — three measurements, all
post-gate (before §17.1 the 3.38 c convergence tax flattened this whole
scale and none of this was measurable):
1. **Converged accuracy identical**: synthetic stepped-note rig centres
   1.92 / 1.98 / 1.98 / 2.03 / 2.05 c at retune 0/1/2/4/6.
2. **Acquisition identical**: settle-to-3c after a note change is
   ~107 ms median, FLAT from 0 through 20 ms — floored by note-change
   detection/confirm, not by the pole. The fast zone buys no speed.
3. **Roughness WORSE at 0**: τ 0 chases hop-level detection jitter
   verbatim. Real voice, rough spans vs Antares: sourceNEW 84 → 74
   (retune 0 → 6); the hard-match take 29 → 26 with worst deficit
   −0.97 → −0.71. Tuning cost of the floor: none measurable on either
   take.

**Antares's zero is not zero**: its fastest setting carries internal
smoothing equivalent to ~4–6 ms of this τ — Sean's blind match
("EchoJay 4–6 sounds like Antares 0") and the roughness curves agree —
so the floor also makes 0 mean what a user arriving from any other
corrector expects. 6 was chosen over 4 BY MEASUREMENT on both takes,
not from the middle of the reported range. Re-baselined hard-match gate
columns are recorded in the commit that landed the floor.

### §17.4 The dial's usable range: useful correction ends ~40 ms (30 Aug 2026)

Full-dial sweep, sourceNEW at hard / vib off / reference 440, gated vs
ungated bleed (AB_UNGATE), rough spans vs Antares / note-centre / within-3c:

    tau    gated spans   ungated spans   centre   within-3c
     0(6)   74 / 0.32s    73 / 0.30s     2.55c    40.4%
     10     77 / 0.31     75 / 0.29      2.37     41.5
     20     71 / 0.28     68 / 0.26      3.23     44.2
     40     64 / 0.27     61 / 0.25      3.55     40.5   <- minimum
     100    80 / 0.33     71 / 0.31      3.74     30.8
     200    87 / 0.37     83 / 0.34      4.57     25.8
     400   100 / 0.41     93 / 0.39      5.54     22.6

**Useful correction ends around 40 ms** — the roughness minimum and the
last stop before the within-3-cents cliff. 40→400 is transparency
CHARACTER (natural lives at 120 with flex/humanize shaping it); 400 is
essentially all scoop — τ never arrives on normal-length notes, and the
larger sustained shifts its glides hold also buy seam charge under the
boundary-scales-with-shift law. Sean's "400 sounds worse" is expected
behaviour on both counts, not a defect. The τ-climb survives REMOVING
the bleed gate entirely (+36 spans gated, +32 ungated from 40→400), so
it is not the gate; the gate's own cost is ~+4 spans, flat in τ, and at
low τ it buys convergence (2.55 vs 3.15 c at dial 0).

**Regime caveat added 1 Sep 2026, per ruling:** this table was measured
at voice_type = low_male. At alto_tenor (the schema default, and Sean's
session), the tau-400 row is now KNOWN to contain the GAP-RESUME FERRY
defect this table never measured - a stale envelope limbo resumed across
sub-200ms gaps, producing off-grid holds (-123c at 5.20s). Do not quote
the tau-400 row as characterising clean behaviour until the resume
re-anchor lands; the SHAPE conclusions (useful zone ~40ms, the tau
climb) survive at both voice types (alto re-measure: 83/77/89 spans at
tau 0/40/400).

**Caveat that must travel with this table (the exp4b lesson):** part of
the long-τ climb is the RULER charging genuine glide motion — cycle
similarity taxes real pitch movement — so the rows above 40 ms are an
UPPER BOUND on actual waveform damage: measurement artefact plus real
seam cost, not damage alone. Do not read the 400 ms row as breakage.

### §17.5 The regime-dependent-constant register (31 Aug 2026 ruling)

Constants whose correctness was VALIDATED IN A REGIME, with that regime
stated - a dial-range, path, or preset change outside it invalidates the
constant, and this list is where to look first:

1. **"restart AT the note" (the confirm snap, curCents_ := inCents)** -
   correct for small tau where the envelope arrives pre-confirm; became a
   173c onset discontinuity when the dial's range grew. Fixed by the
   applied-shift-gated release (envExp 5 default).
2. **The 0-6 ms retune floor (§17.3)** - the dominated zone measured on
   this detector's ~107ms note-change acquisition floor; a faster
   detector re-opens the question.
3. **The 3c drift-bleed cap** - bounds drift only where |r-1| < cap;
   validated for correction-scale shifts with the shift-gated taper.
4. **The 30c applied-shift gate (envExp 5)** - derived as the geometric
   midpoint of [shielded-path ceiling 8.3c, smallest interval 100c].
   VALIDATED IN: this singer, sourceNEW (cross-checked source4 for the
   pending stats), tau in {6, 120, 400}, both vibrato modes, the two
   shift paths as currently routed (shiftPreferred vs legacy). A new
   path, a different fMin voice type, or a shifted kNoteChangeCents
   moves both bracket ends - re-derive, don't re-fit.

5. **voice_type in every measurement (added retroactively, 1 Sep 2026)**:
   entries 1-4 above were ALL validated at voice_type = low_male - the
   instrument stack pinned it, and Sean's session sat on the alto_tenor
   DEFAULT, where a gap-resume-ferried envelope limbo at long tau produced
   a -123c off-grid hold invisible to every panel (the tau-400 5.20s
   defect). §17.4's SHAPE survives at alto_tenor (re-measured: tau 0/40/
   400 -> 83/77/89 spans, centres 2.15/3.45/6.06c - same U-curve, same
   useful-zone conclusion, uniformly slightly worse, worst deficits
   deeper at 400 where the resume defect lives). EVERY measurement from
   now on records its voice_type.

The general hazard, named after five instances: a constant proven in one
regime reads as universal until the regime silently widens. When a dial
range, preset table, path-routing, or VOICE TYPE changes, walk this list.

### §17.6 THE RE-ANCHOR RULE (1 Sep 2026 ruling - a design rule, not a hazard note)

**No state computed before a discontinuity may be applied after it
unless it is consistent with post-discontinuity evidence. Where it is
not, re-anchor; where it is, keep it.**

Operational test (the CORRIDOR): carried envelope position between the
resuming audio and the aim is a partial correction - KEEP it; outside
that corridor it is unexplainable by the new audio - RE-ANCHOR (the
median-of-first-hops primitive).

**The rule's own first draft violated the rule** - preserved here as
the register's most instructive entry: the unconditional form
("always re-anchor") was built and measured 1 Sep 2026, healed the
tau-400 ferry exactly, and REGRESSED every panel row (hard
same-semitone 94.8 -> 85.7, natural 98.4 -> 92.2, spans 56 -> 79,
both voices) by re-anchoring at every 11ms blink the 200ms resume
rule exists to protect - a prescription without a diagnosis, i.e.
instance seven, committed by the rule itself.

Six violations of this one sentence cost, in total, weeks of
measurement (each looked like a different defect):
1. The pre-confirm envelope chase (the note-boundary snap, §17.5 item 1).
2. Seed-from-old-note (experiment (c) of the vib-on rounds: seeding the
   slow track at the old decision's tone reinforced the old vote).
3. Previous-note voting (the vib-on selection lag fixed by (d)).
4. The vibrato-depth estimator ingesting the note-interval excursion as
   "depth" (caught twice in one build).
5. The popout/menu ordering class (the rack-borrow work's inert-mark).
6. The GAP-RESUME FERRY: an unconverged envelope position resumed across
   a sub-200ms gap and applied to the new syllable (-123c off-grid holds
   at long tau, voice-mismatch dependent).

**The fullest worked example (1 Sep 2026, four rounds, terminal):** the
gap-resume ferry - see DEFECT_RESUME_FERRY.md for the chain. The two
transferable lessons, ahead of any fix: (1) the rule's first draft
violated the rule - "always re-anchor" is a prescription without a
diagnosis, and it regressed every panel row; (2) corridor v1 judged
carried state against the SINGLE least trustworthy sample in the signal
- the judgment step must use the same robust evidence as the anchor.
The threshold search that closed it found natural's legitimate carried
offsets (73-215c) fully interleaved with the ferry's (49-1092c): when
no measurable quantity separates the healthy from the defective
population, the honest end is a documented limitation, not another
mechanism.

**COROLLARY (1 Sep 2026 ruling - three for three):** any fix that must
CHOOSE A VALUE at a discontinuity is itself subject to this rule. The
unconditional re-anchor, corridor v1, and envExp 5's release destination
were each written to ENFORCE the rule and each VIOLATED it. Every such
site - note-start seed, gap resume, corridor endpoint, release
destination - takes its value from the one shared robust anchor
(median-of-first-hops), never from a single sample.

**OVERTURN APPENDED TO THE WORKED EXAMPLE (1 Sep 2026):** the ferry
mechanism attribution was WRONG. The pre-gap envelope was healthy
(172.2Hz, converged); the 161.9 was made in ONE HOP by envExp 5's
release easing toward the single sample that raised a (reverting)
pending - a regression in the then-shipped default. The transferable
lessons above survive; the mechanism does not. The methodological
failure, stated plainly: THE FERRY INVESTIGATION'S BASELINE CONTAINED
THE VERY CHANGE UNDER TEST - envExp 5 was the default in every "off"
row of four rounds of panels, so the measurements compared a regression
against itself. Standing instruction, beside "record the voice_type":
STATE THE BASELINE'S FLAG CONFIGURATION IN EVERY PANEL.

Review test for any change touching a seam, gap, resume, onset or
confirm: name the state that crosses it, and name where it re-anchors.
If it has no re-anchor site, it is instance seven. The shared primitive
(PitchCorrect's median-of-first-hops anchor, used at note starts AND gap
resumes) is the standing implementation of this rule for the envelope.
