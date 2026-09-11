# Onset shakiness: what production pitch correctors do that EchoJay does not

Status: research finding, not yet measured against our own code.
Trigger: "the begining sylabls sound shakey no matter what retune speed still."
Date: 2026-09-02.

The τ-independence in that report is the whole finding. Every mechanism we have
chased for the last several rounds — the retune envelope, the boundary snap, the
150 ms cap, the ferry chain — lives in the *target* trajectory, and the target
trajectory is shaped by τ. A defect that is identical at τ=6 and τ=400 is not in
any of them. This document is about what else it can be.

---

## 0. The one-line answer

Production correctors do not try to correct well during an onset. **They stop
correcting.**

Auto-Tune's answer to "the f0 estimate is unreliable at the head of a syllable"
is not a better estimate, a slower envelope, or a smarter target. It is, from
the patent, literally:

> "If pitch tracking has failed, logic step 44 sets **Resample_Rate2 to 1.** and
> Detection_mode to true."

Ratio forced to unity, correction off, re-enter acquisition. Three separate tests
route to that branch: the periodicity criterion (`E−2H ≤ eps·E`, which is the
user-facing **Tracking** knob), an energy floor, and — note this one — "**if the
pitch changes too rapidly**." While `Detection_mode` is true, "Resample_Rate2 is
equal to 1., resulting in no change of output pitch."

We have no equivalent. We correct continuously and try to make the correction
behave. That is the design difference, stated in one sentence.

---

## 1. Why retune speed provably cannot fix this

The shift ratio in every real-time corrector, ours included, is

```
r = f_target / f̂          (Auto-Tune patent: Resample_Raw_Rate = Cycle_period / desired_Cycle_period)
```

With a detector error `f̂ = f_true·(1+e)`, the output pitch is

```
f_out = r · f_true = f_target / (1 + e)
```

so the output error in cents is

```
err = −1200·log₂(1 + e)
```

**Independent of `f_target`.** Whatever we do to the target — smooth it over
400 ms, snap it, humanize it, flex it — the `1/(1+e)` factor survives untouched,
because it enters through the *denominator*. This is the formal reason Sean's
report is τ-independent, and it is why nothing we have shipped has touched it.

Magnitudes, for calibration:

| detector error `e` | output error |
|---|---|
| 1 % | −17.2 c |
| 3 % | −51 c |
| −50 % (octave low) | +1200 c |
| +100 % (octave high) | −1200 c |

Two corroborating observations, both worth more than the algebra:

1. **Auto-Tune smooths the ratio, not the target.** The patent's Retune Speed is
   a one-pole on `Resample_Raw_Rate` — the quotient — with `Decay=0 ⇒
   Rate1 = Raw_Rate`. If jitter entered only through the target, smoothing the
   target would have been the cheaper design. Hildebrand smoothed the place the
   denominator lives.
2. **Autotalent's gate is upstream of the ratio.** Both numerator and denominator
   derive from the *gated* `inpitch`:
   ```c
   if (conf >= mvthresh) { inpitch = tf; minpitch = tf; }   // else hold
   ```
   Which is why it works regardless of the smoothing parameters.

**Second error term we have not been accounting for.** The clean derivation
assumes the ratio is applied to audio whose true pitch is `f_true(t)`. With an
analysis-to-synthesis delay δ:

```
f_out(t) = f_target(t) · f_true(t−δ) / [ f_true(t) · (1+e) ]
```

The extra factor is ≈1 in steady state and vanishes — which is why the clean
result holds on sustains — but during a scoop it is slope×delay. A 200-cent
scoop over 100 ms is 2 c/ms; at δ = 20 ms that is **40 cents of output error
from timing misalignment alone**, before any detector error. Our lookahead is
16.7 / 37.5 / 54.6 ms depending on voice type. At `alto_tenor` (37.5 ms) a
2 c/ms scoop gives 75 cents. This is also τ-independent, and it is *worse* the
more lookahead we add.

---

## 2. Why the estimate is bad at onsets — and why this is a bound, not a bug

Three facts that stack, none of which we can tune our way out of:

**(a) The two-period floor.** YIN's own text: "F0 estimation requires enough
signal to cover twice the largest expected period," with latency floor
`T_max + T`. For a male voice at 82–100 Hz that is 20–27 ms of signal before a
first estimate exists. Measured comparisons (DAFx-10 guitar study) put YIN at
13.4 ms raw, 27.4 ms with post-processing.

**(b) The glottis is not oscillating yet.** Voice-science measurement: acoustic
onset to *sustained* oscillation is ~24–35 ms, and the pre-phonatory phase is
"onset of small **irregular** oscillatory motion." The physical duration of the
attack transient is the same order as the minimum analysis window. **There is no
window placement that sees two clean periods of steady oscillation before the
oscillation is steady.**

**(c) A sweeping f0 disarms YIN's octave protection.** This is the one I did not
know and it matters. Under time-varying f0 the difference function at the true
period no longer reaches zero, and YIN quantifies the residual as growing with
**the cube of window size**. So during a scoop the CMNDF minimum is *lifted* —
it looks like aperiodicity — and may fail the absolute threshold. YIN's stated
fallback when nothing clears threshold: "**If none is found, the global minimum
is chosen instead**," which is precisely the branch with no anti-subharmonic
protection. A scooped entry does not merely add noise to the estimate; it turns
off the mechanism that prevents octave errors.

And the squeeze is analytic, not practical: estimator variance falls with window
length, FM bias grows as W³ with window length. **No window length is
simultaneously low-variance and low-bias during a scoop.**

---

## 3. The PSOLA half — and why `DEFECT_GRAIN_EPOCH_UNITY` should be re-opened

We filed 141 epoch breaks and 15 inversions at shift ratio exactly 1.0 as
out-of-band and left it unchased. The literature says that filing was wrong.

**PSOLA is not transparent at unity.** Moulines & Charpentier's reconstruction
condition is

```
Σ h(t_q − m) = 1   ∀m
```

— the analysis windows, *positioned at the marks*, must sum to exactly one at
every sample. The mark positions `t_q` are arguments of that condition. Ratio
1.0 does not enter it. Break the marks and you break identity at every ratio,
including unity. There is no "unity is safe" property, and the deviation appears
as broadband amplitude modulation plus a phase step at each affected mark.

**The audibility threshold is quantified.** Same source: degradation appears
"when the shift exceeds **30 % of the pitch period**," at which point the output
"begins to sound **hoarse**." At 200 Hz that is 1.5 ms of mark error. For
scale: pitch JND is ~8.6 cents, clinical normal vocal jitter ~0.30 %. A 30 %
period perturbation is roughly 100× normal jitter.

**The artifact signature is exactly "shaky."** From the TD-PSOLA pitch-marking
literature: "one wrongly placed pitch marker will typically make **one
instantaneous pitch period shorter than the true pitch period and the adjacent
period longer**." Short-then-long. That *is* cycle-to-cycle f0 perturbation
injected by the analysis stage — synthetic jitter, not present in the source,
and completely independent of the correction target.

**It concentrates at onsets, and the sources say why.** Epoch detectors "may
give spurious epochs" on unvoiced material, where "the epoch alignment doesn't
make sense"; the V/UV border "can only be located with limited precision because
of the frame based nature of these algorithms"; and a period estimate needs
several periods of evidence behind it, which at an onset do not exist. A
third-party benchmark of 16 shifters scores PSOLA at 0.941 attack-envelope
correlation — second worst, against 0.995 for WSOLA and a plain delay line — and
attributes it in words to "pitch-mark jitter on non-periodic onsets."

**The 15 inversions are worse than the 141 breaks.** Every PSOLA formulation
assumes an ordered mark sequence `t₁ < t₂ < …`. Nothing defines behaviour for a
negative hop. Non-monotonic marks are not degraded PSOLA — they are undefined
behaviour, producing locally out-of-order audio, a click-class artifact. That
should be an assertion in the code, not a counter in a report.

**And the V/UV point applies directly to "beginning syllables."** Word
beginnings are plosives, fricatives, aspiration — genuinely aperiodic for the
first tens of ms. PSOLA's designers handled this with a *second mode*: marks "at
a **constant rate** on the unvoiced portions," and Praat goes further and
straight-copies voiceless intervals rather than running them through the grain
path at all. If we pitch-synchronously overlap-add a plosive, the mark placer
locks onto noise peaks, the mark spacing becomes the noise's peak statistics,
and the *synthesised* f0 during the onset is a random walk. That is a mechanical
account of Sean's exact words, and it does not contain the letter τ anywhere.

---

## 4. What the five other designs actually ship

| Design | Onset mechanism | Separate from retune? |
|---|---|---|
| **Auto-Tune** | `Resample_Rate2 = 1.0` bypass on any of: periodicity fail (`eps` = Tracking), energy floor, "pitch changes too rapidly". Plus a **narrow-band tracker** that can only search ±N/2 lags (N≈8) around the current period, plus quadratic sub-sample refinement of `Lmin`. | Yes — detector state machine, nothing to do with Retune Speed |
| **Waves Tune RT** | **Two time constants**: Speed (15 ms default, within-note) and **Note Transition (120 ms default, into a new note)**. Plus Tolerance→Time: a note "must hold beyond the 50 cents threshold" for up to 300 ms before correction engages. Plus Range, which *bypasses* out-of-range detections. | Yes — three separate controls |
| **MAutoPitch** | **Stabilization**, 0–1000 ms, in the *Detector* panel: "how quickly can the pitch make bigger changes… useful for voice, which **often contains short pieces of inharmonic material, which would normally make the detector jump too quickly**." | Yes — different panel from Speed |
| **GSnap** | Speed = "number of wave repetitions required for pitch-detection" (N-period confirmation); Gate = amplitude floor on the *detector*; Attack = ramp correction in | Yes — three |
| **Autotalent** | `if (conf >= mvthresh) { inpitch = tf; }` — hold last estimate when unvoiced. Confidence exposed as an output port. | Yes — upstream of the ratio |
| **Melodyne** | Pitch *center* averaged over the whole note, with "the beginning and end" explicitly weighted **down**; a constant offset applied across the note | N/A — non-causal |
| **Logic Pitch Correction** | Nothing documented. Deadband + one lag. | — |

Two things to take from the table.

**Every shipping design has detector-side machinery that is a different control
from the retune envelope.** We have one envelope and a jump gate. Melda names
the problem in vendor prose almost exactly as Sean did.

**Nobody uses lookahead to see the attack coming.** Auto-Tune claims none.
Waves is 0–4 ms, period-proportional buffering for the shifter. TC-Helicon's
patent *reduces* latency at onsets ("the latency is lower during instances where
it is more perceptible, such as **during onsets and sudden note changes**"). The
real-time defence is gating and bypass, not prediction. This is worth saying
plainly because our `MIXING` mode's 38 ms of lookahead was the obvious next idea
and the field says it is the wrong one — and §1's δ term says extra lookahead
actively *hurts* during a scoop.

---

## 5. Correction to my own prior instinct

I have been assuming the right move on low confidence is **hold the last good
ratio**. The sources say the canonical answer is **revert to unity**, and I now
think they are right.

- *Revert to unity*: output is momentarily uncorrected. Audibly under-corrected,
  never at a wrong pitch. Error bounded by the singer's own error.
- *Hold last ratio*: output stays corrected with a ratio computed for a pitch the
  singer has since left. At an onset — the exact moment confidence is low — the
  true pitch is moving fastest, so a held ratio is stale in a monotonically
  worsening direction, unbounded as the scoop progresses. And if the last "good"
  frame was an octave error that passed threshold, holding sustains a 1200-cent
  error through the whole low-confidence region.

Autotalent holds; Auto-Tune bypasses. Auto-Tune is the one that sounds right on
attacks. Take the bypass.

---

## 6. Ranked suspects, and the one test that splits them

Four independent τ-independent mechanisms, all live in our design:

**A. PSOLA epoch instability at onsets.** Already measured internally
(141 breaks / 15 inversions at unity), already the strongest documented
match to "shaky," and the only suspect with one of our own numbers attached.

**B. No unity bypass on low detector confidence.** The thing Auto-Tune does that
we do not do at all.

**C. Denominator jitter** — `f_out = target/(1+e)`. Partially addressed if the
shielded shift path (slow term only) is the live path; fully exposed if the
legacy target/f0 path is.

**D. Analysis-to-synthesis delay during a scoop** — δ×slope, 40–75 c at our
lookaheads for a 2 c/ms entry.

**E. No V/UV split in the grain scheduler** — pitch-synchronous OLA on a plosive.
(Arguably a sub-case of A, but a separately fixable one.)

### The discriminating test

**Run the engine in the path with the correction target forced equal to the
detected pitch — unity ratio, everything else identical — and measure the first
150 ms of each word.**

- If onsets are still shaky at unity: the corrector is exonerated entirely.
  Target, retune, flex, humanize, targeting, the boundary snap, the cap — all
  innocent. It is A/E, the grain path, and `DEFECT_GRAIN_EPOCH_UNITY` is the
  ticket.
- If onsets are clean at unity but shaky in normal operation: it is B/C/D, and
  the next split is shielded-path vs legacy-path.

This is one measurement and it halves the tree. It should happen before anything
is built.

The second measurement, which turns Sean's ear into a number: **onset-windowed
cents deviation of output from a smoothed input contour, first 150 ms of each
word, at τ=6 and τ=400 on the same take.** τ-independence is currently a
listening report; it should be a ratio. If the two numbers are within ~10 % of
each other, τ-independence is established and every target-side hypothesis is
formally dead.

Note on the earlier filing: the epoch defect was dismissed as "out-of-band only."
That phrase needs re-reading. If out-of-band meant the analysis excluded
low-confidence or onset frames, the filing excluded exactly the region under
investigation.

---

## 7. Fix candidates, ranked by evidence and cost

1. **Onset bypass** (Auto-Tune's design). On low confidence / energy transient /
   periodicity not established: force ratio 1.0 **and route around the grain
   path** — do not merely set the ratio to 1.0. The literature is explicit that
   running a shifter at unity with bad marks is not identity. Ramp back in over
   the confirm window on exit.
2. **V/UV split in the scheduler.** Constant-rate marks or straight dry copy on
   unvoiced. No pitch-synchronous OLA where there is no pitch.
3. **Monotonicity assertion on the synthesis mark stream.** Inversions are
   undefined behaviour, not degraded behaviour.
4. **Narrow-band tracking + detector-side rate limit.** Bound the detector's
   per-frame excursion structurally (Auto-Tune's ±N/2 window), rather than
   gating large jumps after the fact as `F0JumpGate` does. Melda ships this as
   "Stabilization."
5. **Verify sub-sample period interpolation exists.** Auto-Tune quadratic-fits
   the three points around `Lmin`. Integer-lag periods produce τ-independent
   stepping on their own. Standard YIN includes parabolic interpolation — this
   is a five-minute check, not a project, but it is worth being certain.
6. **Smooth the ratio, not the target** — or confirm the shielded path is live.
7. **Two time constants** (Waves: 120 ms into a note, 15 ms within it). This is
   a feature, not a fix; it is what makes Waves' defaults sound good.

On (7), note there is **no consensus on the musical direction** and we should not
pretend there is: Auto-Tune's Humanize corrects *harder* at the onset and relaxes
on the sustain; GSnap's Attack ramps correction *in* from zero; Waves uses a slow
transition into the note. Three shipping products, three opposite choices. That
makes it a taste knob, and it should be shipped as one — not chosen by us and
baked in.

---

## Sources

Primary — patents
- [US 5,973,252 — Pitch detection and intonation correction (Hildebrand / Antares)](https://patents.google.com/patent/US5973252A/en)
- [US 2014/0180683 — Dynamically Adapted Pitch Correction Based on Audio Input (Harman / TC-Helicon)](https://patents.justia.com/patent/20140180683)
- [US 6,049,766 — Time-domain time/pitch scaling with transient handling](https://patents.justia.com/patent/6049766)

Primary — manuals
- [Auto-Tune Pro X User Guide 10.0](https://antares-web-frontend.sfo3.cdn.digitaloceanspaces.com/documentation/pdfs/Auto-Tune_Pro_X_User_Guide_10.0.pdf)
- [Waves Tune Real-Time User Guide](https://assets.wavescdn.com/pdf/plugins/tune-real-time.pdf)
- [MAutoPitch documentation](https://www.meldaproduction.com/download/documentation/MAutoPitch_intro.pdf)
- [GVST GSnap documentation](https://gvst.uk/Downloads/GSnap)
- [Melodyne 5 — Pitch Tool](https://helpcenter.celemony.com/M5/doc/melodyneStudio5/en/M5tour_ToolPitch_2?env=standAlone)
- [Logic Pro — Pitch Correction parameters](https://support.apple.com/guide/logicpro/pitch-correction-parameters-lgcef2835c07/mac)

Primary — academic
- [de Cheveigné & Kawahara — YIN (JASA 2002)](http://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf)
- [Mauch & Dixon — pYIN (ICASSP 2014)](https://webspace.eecs.qmul.ac.uk/s.e.dixon/pub/2014/MauchDixon-PYIN-ICASSP2014.pdf)
- [Moulines & Charpentier — PSOLA](https://courses.physics.illinois.edu/ece420/sp2019/5_PSOLA.pdf)
- [Röbel — A new approach to transient processing in the phase vocoder (DAFx-03)](https://www.mp3-tech.org/programmer/docs/dafx32.pdf)
- [Driedger & Müller — A Review of Time-Scale Modification of Music Signals](https://www.cs.bu.edu/fac/snyder/cs583/Literature%20and%20Resources/AReviewOfTimeScaleModification.pdf)
- [Rosenzweig et al. — Adaptive Pitch-Shifting (DAFx 2020-in-21)](https://dafx2020.mdw.ac.at/proceedings/papers/DAFx20in21_paper_11.pdf)
- [Drugman et al. — Detection of Glottal Closure Instants: A Quantitative Review](https://arxiv.org/pdf/2001.00473)
- [ESOLA: Epoch-Synchronous Overlap-Add](https://arxiv.org/pdf/1801.06492)
- [von dem Knesebeck & Zölzer — Comparison of Pitch Trackers for Real-Time Guitar Effects (DAFx-10)](https://dafx10.iem.at/papers/VonDemKnesebeckZoelzer_DAFx10_P102.pdf)
- [Meier et al. — Pitch Estimation in Real Time: Revisiting SWIPE with Causal Windowing (CMMR 2025)](https://www.audiolabs-erlangen.de/content/05_fau/professor/00_mueller/03_publications/2025_MeierSSMB_RealTimeSWIPE_CMMR_ePrint.pdf)
- [Robust pitch marking for prosodic modification of speech using TD-PSOLA](https://www.academia.edu/14815667/ROBUST_PITCH_MARKING_FOR_PROSODIC_MODIFICATION_OF_SPEECH_USING_TD_PSOLA)
- [Relationship between acoustic voice onset and oscillatory onset (PMC5395360)](https://pmc.ncbi.nlm.nih.gov/articles/PMC5395360/)

Source code / secondary
- [olilarkin/autotalent — autotalent.cpp](https://github.com/olilarkin/autotalent/blob/master/autotalent.cpp)
- [audiojs/shift — pitch-shift algorithm benchmark](https://github.com/audiojs/shift)
- [Valhalla DSP — Auto-Tune, autocorrelation, and seismic analysis (Hildebrand's own 2016 clarification)](https://valhalladsp.com/2009/05/21/auto-tune-autocorrelation-and-seismic-analysis/)
- [Sound On Sound — Waves Tune Real-Time review](https://www.soundonsound.com/reviews/waves-tune-real-time)
- [Praat manual — overlap-add (PSOLA)](https://www.fon.hum.uva.nl/praat/manual/overlap-add.html)

Caution: [Antares' own "Science Behind Auto-Tune" blog](https://www.antarestech.com/blog/the-science-behind-auto-tune)
claims the plugin "employs the Fast Fourier Transform." This contradicts both the
patent and Hildebrand's own statement. Do not cite it on internals.
