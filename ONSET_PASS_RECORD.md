# Onset-shakiness pass record (2 Sep 2026) — corrected per review

Companion to ONSET_SHAKINESS_RESEARCH.md. Voice alto_tenor throughout
(Sean's setting). Ruler: tools/pitch_onset_probe — 24 onsets on sourceNEW
(12 on dry.wav), 150 ms windows, per-hop cents track.

## The table, with the Antares rows and contrast ratios

    row                        onset jitter   sustain jitter   CONTRAST
    SOURCE (sourceNEW)             5.22            1.41          3.70x
    Antares @ Retune 400           5.70            1.31          4.35x
    EchoJay full chain tau6        5.68            0.86          6.60x
    EchoJay full chain tau150      6.62            0.83          7.98x
    M2 "unity" (splice path)       5.22            1.41          [INVALID - see below]
    M3 unity, grain path           5.47            1.27          4.31x

    old take (dry.wav):
    SOURCE                         8.54            5.47          1.56x
    Antares @ Retune 0             9.51            5.66          1.68x

Antares settings for both rows were READ FROM THE UI (the 1 Sep
matched-settings block; the old trio's retune-0 configuration is the
hard-match gate's documented setup). NOTE the retune mismatch on the NEW
take: antaresNEW is Antares at THEIR 400; our rows are tau 6/150. No
matched-retune Antares render exists on sourceNEW; one at their Retune 0
would nail the magnitude comparison and is requested, not required.

## The branch verdict (neither branch exactly; nearer "universal")

1. Antares NEVER cleans onsets below the source floor: 5.70 vs 5.22 (+9%)
   on sourceNEW, 9.51 vs 8.54 (+11%) at Retune 0 on the old take. The
   research doc's section 4 menu (bypass, narrow-band tracking,
   stabilization) produces AT BEST source-level onsets - which our M3
   grain path already measures (within 5% of source). "Antares actively
   cleans onsets" is REFUTED.
2. The contrast-growth direction is universal; the MAGNITUDE is ours:
   Antares +18%/+8% over source vs our +78%/+116%. The difference lives
   entirely in the SUSTAIN column - we smooth sustains to 0.83-0.86
   c/hop where Antares (at the settings on file) leaves 1.31-5.66. The
   headline stands as a PRODUCT question about proportion: our onsets are
   source-grade like everyone's; our sustains are polished harder, which
   spotlights the take's own onset instability.

## Amendments (per review, binding on citations of the earlier pass)

**A1 — M2 is INVALID as a discriminator: it measured the source with
extra steps.** Why, from the code, not from the candidate list: at shift
= 0.0 the in-band splice-resampler is the EXACT IDENTITY BY DESIGN -
drift accumulates at (r-1)=0, readInterp at integer positions returns
input samples bit-exactly, and seams crossfade two identical signals.
The engine's own comment records it: "ZERO at unity... by construction,
the identity." Not shiftPreferred declining, not the g<=0 rule, not
methodMix routing: a designed, documented property (and a good one - it
is why in-band preserve is transparent at rest). But it means a
nominal-unity request through the splice path cannot exercise the
machinery. THE EXONERATION OF THE CORRECTOR STANDS ON M3 ALONE (grain
path, a real render, onset jitter within 5% of source).

**A2 — the wobble column is in CENTS (mean |deviation from a 5-hop
median|) and sits 45-80x BELOW the 8.6c pitch JND.** It is marked
below-audibility; the 0.11 -> 0.19 "near-doubling" carries no weight;
every surviving conclusion rests on the jitter column.

**A3 — the M1 conclusion is corrected to two claims.** The TOTAL onset
jitter is tau-invariant (5.68 vs 6.62 against a 5.22 floor) - which is
what kills the target-side hypotheses. OUR CONTRIBUTION (total minus
floor) is NOT: +0.46 at tau6 -> +1.40 at tau150, tripling with tau,
direction as expected from the delta x slope term (research doc section
1) - a stale ratio applied to a fast-moving onset.

## Filed, not chased (cross-referenced both ways)

Every word beginning is a dry->wet seam BY CONSTRUCTION (the V/UV split
keeps unvoiced bit-exact dry), and DESIGN_SEAM_RESIDUAL.md documents a
per-boundary pitch step at exactly those seams - a tau-independent onset
mechanism living inside a filing already accepted as by-design. The seam
population coincides with the region under investigation. Not re-opened:
the Antares rows show its onsets carry the same class of charge.

---

# Round 3 (2 Sep 2026): the jitter-neutrality reframe — M2 + cross-check done, M1 specified and BLOCKED on a bounce

Hypothesis under test: Antares is jitter-neutral by architecture (delay-line
repeats whole original cycles); we are not (TD-PSOLA resynthesises periods at
our estimated marks). Research doc section 3: PSOLA 0.941 attack-envelope
preservation vs 0.995 for a delay line.

## M1 — the matched bounce: REQUIRED FROM SEAN, cannot be produced here

Antares renders only exist from Sean's Logic session; nothing on disk is a
fast-retune Antares bounce of sourceNEW. **The spec, derived not picked:
Retune Speed 0** — the 6ms-floor calibration (§17.3) measured Antares' 0 as
equivalent to our 4–6ms τ, so their 0 is the fair match to our τ6. Everything
else identical to the matched-settings block: Input Type Alto-Tenor, Key D
minor, Detune 440, Flex-Tune 0, Humanize 0, Natural Vibrato 0, Tracking 50,
Transpose 0, Formant on, Mix 100 — read off the UI at bounce time and
recorded, never assumed. Run tools/pitch_onset_probe on the result; the
sustain-jitter cell decides: stays near 1.3 → architectural, hypothesis
confirmed; drops toward 0.86 → the reframe collapses into a settings
mismatch and closes.

## M2 — tuning separated from jitter (tools/pitch_sustain_tuning, sustains only, D minor/440)

    row                 n    off-grid med   in-scale   improve   same-semi
    SOURCE             170       6.0c         99.4%      —         99.4%
    ANTARES @400       170       3.7c        100.0%     75.3%     100.0%
    ECHOJAY tau6       165       1.9c        100.0%     82.4%     100.0%

**Verdict, in the ruling's words: our tuning accuracy BEATS Antares and our
jitter is lower — it is a win carrying a separate, untracked cost.** Not the
pure-cost branch. Caveat carried: the Antares column is at THEIR Retune 400
(slow — looser tuning is expected at that setting); the M1 bounce retests
this at their 0 before the verdict is treated as settings-fair.

## CROSS-CHECK — the de-jittering is REAL at waveform level, not a YIN artifact

tools/pitch_period_hist: NO tracker in the loop — source-picked sustain
windows applied to all three files, band-pass at the window's median f0,
rising zero-crossing intervals = raw period lengths, deviation from window
median in cents:

    windows x periods        SOURCE          ANTARES @400     ECHOJAY tau6
    4 x 55  (100ms wins)     IQR 12.3c       IQR  9.3c        IQR  7.5c
                             within5c 50.9%  58.2%            70.4%
    18 x 180 (80ms wins)     IQR  9.8c       IQR 11.5c        IQR  8.4c
                             within5c 51.1%  49.2%            57.4%

EchoJay's raw period-length distribution is the NARROWEST in both runs (more
mass within 5c of the window median in both); Antares STRADDLES the source
(9.3 vs 12.3 one run, 11.5 vs 9.8 the other — neutral within this ruler's
noise). The tracker's 39% jitter reduction shows up smaller here (IQR −14 to
−32%) because raw zero-crossing periods carry a measurement-noise floor the
tracker's median smooths — but the ordering and the direction are the same
ruler-independently. **The de-jittering is genuine period-sequence
regularisation, and Antares' waveform-level neutrality is consistent with
the architecture hypothesis — which M1 alone can confirm or kill** (a fast
Antares could still regularise via its correction loop even with a neutral
shifter; that is exactly the axis the bounce separates).

Nothing built. No flags. Awaiting the Retune-0 bounce.

---

# Round 4 (2 Sep 2026): antares_retune0_NEW OVERTURNS the onset conclusion

The bounce: /Users/SeanD/Desktop/antares_retune0_NEW.wav (2 Sep 16:31),
measured independently in the cloud container first, confirmed here.

**Settings verification, honestly stated:** NO screenshot exists for this
bounce. The newest Antares screenshot on disk (1 Sep 18:41) shows Retune 20 /
Key C CHROMATIC - it describes an earlier session moment, not this bounce.
Audio-level evidence: the chromatic detector (share of voiced hops sitting on
a NON-D-minor chromatic tone) reads 0.6% - exactly the source's own rate - so
the bounce behaves as D-minor-compatible; and its onset off-grid (med 1.94c
here, 1.15c in the container) is only reachable at a very fast retune. The
settings are CONSISTENT with the Retune-0/D-minor spec but not
screenshot-verified; a screenshot at the bounce settings would close the gap.

## JOB 1 - my rulers vs the container's (off-grid to D natural minor, 440)

    onset off-grid          med     p75     p90    all-voiced <5c
    SOURCE       container  6.63   16.31   36.54      38.8%
                 here       8.49   16.64   30.74      39.8%
    ANT_400      container  5.09   12.44   33.35      45.2%
                 here       6.42   13.42   27.19      46.9%
    ANT_0        container  1.15    3.25    9.51      66.1%
                 here       1.94    6.15   17.52      76.3%
    EJ_ignoreoff container  6.03   10.38   21.78      44.7%
                 here       6.56   10.21   16.69      48.9%
    EJ_ignoreon  container  7.07   17.77   36.16      37.8%
                 here       5.71   10.72   23.23      45.0%
    EJ tau6 current engine  4.95    8.74   18.41      59.9%   (no container row)

No material disagreement: ordering identical everywhere that matters, the
ANT_0 collapse reproduced (med improves 3.3x here vs 4.4x there; the p90
ratio is 1.55x here vs 3.5x there - different trackers and onset windows;
neither ruler is "wrong", and both carry the same conclusion). **Antares at
Retune 0 corrects onsets hard and accurately: onset med 1.94c vs our
current-engine best 4.95c (2.6x tighter), all-voiced <5c 76.3% vs 59.9%.**

Jitter/contrast rows (tools/pitch_onset_probe, this machine's ruler):

    row                        onset jitter   sustain jitter   CONTRAST
    ANT_0                          4.84            1.13          4.27x
    EJ_ignoreoff (29 Aug bounce)   7.07            0.97          7.30x
    EJ_ignoreon  (29 Aug bounce)   7.18            1.61          4.47x

ANT_0's onset jitter is BELOW the source floor (4.84 vs 5.22, -7%) - the
first row ever measured under it.

## JOB 2 - the EchoJay row identity, established

The container's EJ rows are Sean's **29 Aug HARD-preset bounces** - an engine
predating the vib-on (d) fix (installed ~31 Aug), the boundary-snap work, and
the retune cap. Measurably stale: onset jitter 7.07/7.18 vs the current
engine's 5.68 at the same operating point. **The matched tau6 render EXISTS:
pf2_hard_v1_e0.wav** - current engine, rendered by this session's harness
with settings known by construction (alto_tenor, D minor, 440, tau 6ms,
IGN VIB on, envExp 0). No new bounce is required for the analysis; a fresh
Logic bounce at the current install is an EAR decision, not a data need.

## JOB 3 - VOID, MEASURED AT UNMATCHED RETUNE

The following round-2 conclusions are marked void; each rested on ANT_400:
  - "our onsets are Antares to within noise" (5.68 vs 5.70)   VOID
  - "the onset column is closed / matched, nothing to do"     VOID
  - "'Antares actively cleans onsets' is REFUTED"             VOID - at 0 it
    cleans them below the source floor (jitter 4.84 vs 5.22) and 3x tighter
    to grid than our best
  - "section 4's menu produces at best source-level onsets"   VOID
**Section 4 of ONSET_SHAKINESS_RESEARCH.md is LIVE again**: the
bypass-on-low-confidence design, the narrow-band tracker, and the
detector-side rate limit are all back on the table as things Antares may be
doing at onsets that we are not.

**Also void by the same mismatch - the round-3 M2 verdict.** At matched
retune, ANT_0 sustains: off-grid med 0.8c, improve 97.1% - against our 1.9c /
82.4%. The verdict flips to WORSE than the ruling's second branch: **our
jitter reduction is pure cost, buying nothing - we smooth sustains harder
than Antares (0.86 vs 1.13) while tuning them less accurately (1.9c vs
0.8c).** The primary finding of this round, ahead of the jitter question:
at matched retune Antares beats the current engine on BOTH onset and sustain
tuning while removing LESS of the singer's micro-variation.

## JOB 4 - the jitter-neutrality hypothesis: survives, weakened; now secondary

ANT_0 sustain jitter **1.13** - nearer 1.3 than 0.86. The reframe does NOT
collapse into a settings mismatch: even at its fastest setting Antares
removes only 20% of the source's sustain jitter (1.41 -> 1.13) where we
remove 39% (1.41 -> 0.86). But strict architectural neutrality is dead too -
its correction loop regularises somewhat with speed (1.31 at 400 -> 1.13 at
0) and its onsets go BELOW the source floor at 0. Verdict, plainly: **Antares
de-jitters less than us at every setting it has, and the gap is real; but
the onset tuning gap (JOB 3) is the live question and this one is secondary.**
Both takes: on the old creaky low_male take, fast Antares removed nothing
(5.47 -> 5.66, +3%) - the neutrality picture is material-dependent; no valid
old-take EchoJay cell exists (the old echojay.wav is a pre-splice artifact
and the take's key is unrecorded, so its grid metrics are undefined).

## Cross-reference filed (not re-opened)

ignore-vibrato OFF tunes better than ON in both rulers (container onset p75
12.77 vs 18.71; here sustain jitter 0.97 vs 1.61, <5c 48.9% vs 45.0%) - on
bounces that PREDATE the (d) fix, i.e. consistent with
DEFECT_VIBRATO_ON_TUNING_COST as it stood then. Noted there; not re-opened.

Nothing built. The next decision needs a ruling on section 4's menu.

---

# Round 5 (2 Sep 2026): section-4 ruling FINAL + the convergence measurement

## The ruling, recorded as decided (FINAL per Sean's confirmation message)

**KILLED as primary candidate: low-confidence bypass.** Arithmetic: a bypass
sets ratio 1.0, so its onset output IS its input - source-level 5.22 by
definition. It cannot produce ANT_0's 4.84. Its whole achievable range moves
us from 5.68 toward 5.22; Antares is already past that. Kept filed as a
possible safety net for gross detector failures only.

**PROMOTED: hard snap to a constant target, with a detector accurate enough
to land it.** At Retune 0 there is no ratio smoothing; during a note the
output is pinned to the scale tone and the only residual is detector error.
ANT_0's onset jitter 4.84 and off-grid 1.15-1.94c are ONE finding. The
narrow-band tracker does not smooth the output - it keeps detector error
small enough that a hard snap lands on the RIGHT note. We also snap at tau6;
ours does not land. The question: why is our target wrong or late in the
first 150ms when theirs is not.

**Settings flag CLOSED:** Sean confirms Retune Speed 0 by hand (recorded in
REFERENCE_SET.md; no screenshot; audio evidence concordant). Comparison
fair per §17.3.

## The measurement — tools/pitch_onset_converge (10c threshold, 150ms windows)

NEW take (alto_tenor, D minor, 24 onsets):

                      first<10c      HOLD<10c        wrong-tone      settles
                      (median)       to window end   dwell (median)  wrong
    SOURCE            24/24 16.0ms   17/24 109.3ms   16 on, 21.3ms   0/24
    ANT_0             24/24  8.0ms   23/24  26.7ms   18 on, 26.7ms   1/24
    EJ tau6 current   22/24 10.7ms   20/24  77.3ms   18 on,  5.3ms   1/22

    median |off-grid| trajectory: ANT_0 collapses to 1-2c by ~45ms and PINS
    there. EJ descends to a 3-5c FLOOR by ~55ms and floats, with excursions
    past 10c recurring until ~77ms. Source floats at 5-11c all window.

The in-process render (SELF:6) reproduces pf2_hard_v1_e0 line for line -
the round-4 row identity is now proven, not argued.

**tau->0 row: VOID as a discriminator.** SELF:0 output is bit-identical to
SELF:6 - kRetuneFloorMs=6 clamps inside PitchCorrect, so no sub-floor tau is
reachable without an engine change (next pass's territory, not this one).

OLD take (low_male; grid CHROMATIC - the take's key was never recorded, so
D-minor metrics would be fiction; 11 onsets):

    SOURCE            11/11  2.7ms    3/11 136.0ms   11 on, 26.7ms   0/11
    ANT old fast      11/11 16.0ms    7/11 125.3ms   11 on, 26.7ms   2/11
    EJ tau6 current   11/11 13.3ms    9/11 130.7ms    9 on, 29.3ms   2/11

On creaky low_male material NEITHER plugin pins (both hold >2s worth of the
window off-grid); EchoJay's trajectory is the better of the two through most
of the window. The onset gap is a CLEAN-MATERIAL phenomenon.

## The discrimination — (d), a mix, with the dominant term named and one term the menu did not contain

- **(a) wrong target: DEAD.** Settles on other-than-source-intent: 1/22 vs
  1/24. Detector accuracy at the note-choice level is not the problem.
- **(b) late acquisition: MINOR.** First-touch 10.7ms vs 8.0ms - a +2.7ms
  gap. kNoteConfirmMs=25 does NOT appear as a 25ms penalty at onsets on
  this data; the acquisition sixth of the window is not where we lose.
- **(c) target re-decided mid-note: NOT SUPPORTED as defined.** A
  re-decision would show as dwell near OTHER tones; our wrong-tone dwell is
  5.3ms median - one fifth of Antares' own 26.7ms (their hard snap grabs
  the approach tone early and audibly sits on it; we glide through).
- **DOMINANT (the unlisted term): hold failure around the CORRECT tone -
  residual tracking leak.** We reach the right tone almost as fast as
  Antares, then keep crossing 10c until 77.3ms (vs their 26.7ms) and floor
  at 3-5c (vs their 1-2c). Quantitatively consistent with what a 6ms
  one-pole follower must do: deviations on 30-80ms timescales (the
  singer's onset excursions - the source's own mid-window median sits at
  6-11c) pass a tau=6ms follower at ~60-85% amplitude; 6-9c x ~0.6 = our
  observed 3-5c floor, and 12-15c source excursions leak out past 10c.
  Antares' fast mode is NOT a lagged follower - it subtracts the full
  measured deviation (the promoted hard snap), leaving detector-error-sized
  residual (1-2c). The §17.3 calibration equated the two on step
  convergence, which both shapes pass; onset excursion tracking is where
  they differ.

So the proposal's target, when asked for: not acquisition, not note choice -
the RESIDUAL SHAPE of retune-fast. Sub-floor/snap behaviour at the dial's
fast end is the thing to propose, with the hold-time and floor numbers above
as the acceptance axes, and the old take as the do-no-harm axis (we are
currently BETTER than Antares there; a snap must not break creak).

## The M2 flip's implication — recorded as the current best account

At matched retune Antares tunes sustains to 0.8c while removing only 20% of
sustain jitter; we tune to 1.9c while removing 39%. Better on BOTH axes at
once - so tuning accuracy and microstructure preservation are NOT in
tension, and our jitter loss buys nothing. Combined with its tau-invariance
(0.86 at tau6 vs 0.83 at tau150), that locates the loss in the SHIFTER, not
the correction loop - where every round of effort so far has gone.

**HYPOTHESIS, NOT MEASURED** (labelled as such per ruling): TD-PSOLA
resynthesises periods from our estimated marks and inherits our mark
regularity; cycle repeat/delete reuses the singer's own cycles and preserves
theirs. Plausible and consistent with everything measured; not yet tested.

## Next (not this pass)

A proposal with an acceptance bar, both takes, and a written falsification
condition committed with the flag BEFORE any code.

---

# Round 6 (2 Sep 2026): STEP 1 CODE VERIFICATION — the premise is REFUTED for the measured configuration. Report and stop.

Ordered: confirm the leak is the shielded shift path emitting the slow term
only, leaving osc = inCents - slowCents_ in the output; stop if it is
somewhere else. It is somewhere else.

## What IS confirmed (the hypothesis, on the path it describes)

EedPitchCorrect.h:626/636/775-777, verbatim: osc = inCents - slowCents_;
the envelope glides the NOTE alone; on the SHIFT-PREFERRED path (natVib ~=
100 only, per shiftPreferred() at :869) the emitted shift is shiftSm_ - the
SLOW part - and osc survives by algebraic cancellation. On THAT path,
vibrato and onset shake are one signal and both are preserved by design.
Every word of the hypothesis is true THERE.

## Where the measured leak actually lives

Every measured row in this pass (natVib 0, IGN VIB on, flex 0, hard) rides
the LEGACY path - shiftPreferred() is FALSE at natVib 0. On that path:

  1. The target is osc-FREE. At flex 0: wanted = degreeCents - noteCents,
     aim = noteCents + wanted = degreeCents exactly (:642,:675). Mid-note
     (no pending) aim is CONSTANT; curCents_ settles onto it within ~4*tau
     (~24ms at tau6) and stays. targetCents_ = curCents_ (:818). No osc
     term reaches the target.
  2. The engine already DIVIDES OUT the singer's deviation, per sample:
     in-band the preserve path rides the splice resampler, ratio =
     target/f0Here with f0Here read per-sample from the lag-compensated
     f0 ring (EedPsolaEngine.h:741-757). Output pitch =
     f0_true(t) * target / f0Here(t).

So mid-note the legacy path is ALREADY A SUBTRACTOR, and the retune
constant does not appear in its transfer at all. The residual is exactly:

     leak(t) = degree * f0_true(t) / f0Here(t)

**the freshness error of the detected f0** - hop sampling (5.3ms) plus the
detector's estimation group delay. A deviation with a 30-80ms timescale
against a ~15-20ms-stale estimate leaves 60-85% standing - the same
magnitude the tau-follower model predicted, which is why round 5's
attribution fit numerically. Round 5's MECHANISM attribution (tau6 one-pole
leak) is SUPERSEDED by this code reading: the magnitude match was a
coincidence of two models with similar transfer at these timescales.

## Three measurements already on record corroborate staleness, not shape

  1. The tau->0 row (round 5): bit-identical output. Now doubly explained -
     mid-note, tau is not in the transfer AT ALL on this path.
  2. Tau-invariance of the sustain jitter removal (0.86 at tau6, 0.83 at
     tau150): a follower's removal would scale with tau; a stale
     subtractor's does not.
  3. PATH_UNIFICATION Q1 + cuts 1-5: forcing the shift path with the
     ring-aligned fast term at k=0 NEVER measured better than the legacy
     division (13.1-16.3c vs legacy 10.5c) - expected, because fastFactor's
     reference (f0Here/slowHere) is built from the SAME ring: the ring
     subtractor's theoretical best equals the legacy division we already
     run.

## Why step 3's vehicle cannot meet step 2's bar

The ring-aligned fast term subtracts the DETECTED deviation. The measured
configuration already subtracts the detected deviation. The bar (hold 77.3
-> ~27ms, floor 3-5c -> 1-2c) requires reducing f0_true/f0Here - detector
freshness at onsets - which no exponent on ring-derived quantities can
touch. Scheduling k along the dial would convert the natVib~=100 SHIFT path
to a subtractor (real, but that is not the configuration Sean's complaint
or any measured row is in) and would leave the legacy rows where they are.

## The corrected mechanism statement, for the next ruling

Antares' fast mode and our fast mode are BOTH subtractors. Theirs snaps
with a fresh estimate; ours with a stale one. The section-4 ruling's own
promoted mechanism contains the fix's location: "the narrow-band tracker...
keeps detector error small enough that a hard snap lands" - detector-side
freshness at onsets (narrow-band re-estimation, or predictive deviation
estimation), not shifter-side algebra. The 1-2c-vs-3-5c floor gap and the
26.7-vs-77.3ms hold gap are measurements OF OUR DETECTOR'S onset group
delay as seen through a subtractor.

Stopped per the step-1 instruction: no acceptance bar committed (it assumed
the plan), no flag, no engine code.

---

# Round 7 (2 Sep 2026): the freshness measurement - built, three ruler
# defects caught en route, verdict INSTRUMENT-LIMITED. Prediction neither
# passed nor failed; stopped rather than over-claimed.

## Record-keeping ordered with the round (done first)

- Round 6 corroboration item (a), the tau->0 bit-identity, is marked
  NOT-EVIDENCE here: it is explained entirely by kRetuneFloorMs=6
  clamping - no sub-floor tau was reachable, so it cannot testify about
  whether tau is in the transfer. Circular. The staleness case stands
  unweakened on (b) tau-invariance of sustain jitter removal and (c)
  PATH_UNIFICATION's measured cuts; either alone is sufficient.
- WHY THE WRONG MODEL FIT (standing caution, beside the "-3.40c = the
  3-cent bleed cap" numeric coincidence): a windowed f0 estimator and a
  one-pole follower have similar low-pass transfer at 30-80ms
  timescales; round 5's arithmetic closed on the wrong mechanism at
  60-85% because of it. A MATCHING MAGNITUDE IS NOT A MATCHING MECHANISM
  when two candidate models share a transfer shape over the measured
  band.
- CORRECTED MECHANISM (the ruling's statement, recorded): both plugins
  are subtractors; ours divides by a SMEARED estimate. A ~40ms analysis
  window during a sweeping onset returns a low-passed trajectory, and
  lag compensation aligns that estimate in time WITHOUT making it less
  smeared - time-alignment cannot recover smoothing. The 3-5c onset
  floor is the high-frequency part of the singer's real onset motion
  that the window removed and the subtractor then faithfully passed
  through.

## The tool and its three caught defects (each documented at the site)

tools/pitch_f0_freshness: reference = per-cycle zero-phase band ZC
(self-test bound on a known 200Hz + 25c/40ms scoop + 10c vibrato:
median 0.87c, max 1.61c); ring reconstruction = the render harness's
exact detection chain with the ring's write rule (gated hop value
attributed lag back). Corrections caught by cross-checks, in order:
  1. Raw per-cycle reference read the SINGER'S OWN cycle jitter as
     estimator error (6.3c sustains vs known 1.9c output tuning) ->
     3-cycle median.
  2. The measured output floor was read through the fine tracker's own
     smoothing -> residual resampled to the fine grid + 24ms moving
     median.
  3. The gate reconstruction queried inputPeriodicity against an UNFED
     engine ring -> engine now processes slice-by-slice exactly as the
     harness does.
  4. Cross-ruler timing: a net alignment error manufactures
     |slope|*Delta cents under vibrato -> Delta scanned on sustains
     (NEW: -4ms; OLD: +7ms, which alone removed 7.2c of the OLD take's
     apparent error - the alignment term was most of it there).

## The numbers, both takes, at best alignment

    NEW (alto_tenor):  onsets med 7.40c p75 13.43 p90 27.15 | sustains med 5.81c
    OLD (low_male):    onsets med 8.17c p75 22.87 p90 50.95 | sustains med 5.84c

## Why this is INSTRUMENT-LIMITED, not a verdict

The sustain reading is the built-in consistency check, and it fails:
the ACTUAL rendered output tunes sustains to 1.9c median off-grid
(round 3, tools/pitch_sustain_tuning) - impossible through a ring that
were truly 5.8c wrong at sustains. So the ruler pair carries ~4-5c of
un-modelled real-material error (candidates: ZC bias under formant
motion that a static-spectrum self-test cannot expose; residual
reconstruction infidelity vs the real render's ring; non-commutation of
tracker and resampler). The prediction band under test (1-2c vs 3-5c)
sits BELOW the instrument's demonstrated error on real material.
Therefore: PREDICTION NEITHER PASSED NOR FAILED. The onset-minus-sustain
differential (+1.6c NEW, +2.3c OLD) points the predicted direction but
is within the instrument's own inconsistency and is not claimed.

## What would discriminate (named for the next ruling, not built)

  1. A DEBUG RING TAP: dump the real render's f0 ring at read time
     (debug-only accessor in EedPsolaEngine, same family as dbgBridge_)
     - removes the reconstruction leg entirely; the ruling's no-engine-
     change constraint means this needs approval first.
  2. THE DIFFERENTIAL FORM: predicted output off-grid (from the tapped
     ring) vs measured output off-grid through ONE tracker on the SAME
     render - ruler biases cancel by construction.

Stopped here per the prediction protocol: the result is not the
confirmation branch, and claiming it would be fitting the conclusion
to the wish.

---

# Round 8 (2 Sep 2026): tap built and PROVEN NEUTRAL; the differential
# form's own sustain gate FAILS - the model of the engine is wrong.
# Stopped at the gate, per protocol. Onset numbers not read.

## Standing caution filed (ordered): DELTA x SLOPE AS INSTRUMENT ERROR

Cross-ruler timing manufactures |slope| x delta cents - the OLD take's
7ms alignment alone produced 7.2c of phantom error (round 7). This is
the SAME delta x slope term ONSET_SHAKINESS_RESEARCH.md section 1
identifies as a DESIGN error source, reappearing as an INSTRUMENT error
source: any two rulers with a timing offset fabricate error
proportional to pitch slope - which means they fabricate most exactly
where we are looking (onsets). Filed beside magnitude-is-not-mechanism
and the bleed-cap numeric coincidence.

## The tap (approved engine change, debug-only)

PsolaEngine::debugRingTap - change-triggered (inPos, f0Here, target)
at the splice read site, dbgBridge_ family, off by default.
**BIT-IDENTITY VERIFIED BEFORE ANY TAPPED DATA WAS READ:** tap-off vs
tap-on renders byte-compared - IDENTICAL on both takes (636096 samples
NEW / 393216 OLD; 3912 / 3139 tap entries).

## The differential form and its gate (tools/pitch_residual_closure)

One tracker T, prediction = 1200*log2(T(source)/f0Here_tapped),
measurement = 1200*log2(T(output)/target_tapped). No reconstruction
leg, no second ruler.

    SUSTAIN GATE (NEW, alto_tenor, D minor, tau6, n=209):
      predicted med 5.34c p75 8.39c | measured med 1.74c p75 3.67c
    SUSTAIN GATE (OLD, low_male, chromatic, n=354):
      predicted med 29.09c | measured med 25.29c (creak-dominated,
      both sides; carries no independent weight)

**GATE FAILED.** The measured leg lands exactly on the known 1.9c
(1.74c) - the instrument's measurement side is sound. The PREDICTED leg
reads 3x higher with the REAL ring values, no reconstruction involved.
Therefore the naive model f_out = f_in * target / f0Here is WRONG about
this engine: the output sits ~3.6c CLOSER to target at sustains than
its own instantaneous ratio accounts for. The engine outperforms its
own ratio.

## The finding (named, not chased - the gate says stop)

Some mechanism between the ratio and the emitted audio pulls pitch
toward target beyond the instantaneous ratio. The size-consistent
candidate, NAMED UNDER THE magnitude-is-not-mechanism CAUTION and not
claimed: the shipped drift-bleed (<=3c cap, tau 100ms) - a feedback
term steering accumulated drift toward zero, absent from the model;
the observed gap is ~3.6c against its 3c cap. Other unmodelled terms:
the 2ms ratio slew, splice re-anchoring. Establishing WHICH is the
next instrument question, and it changes the onset prediction too: a
tau-100ms feedback helps sustains far more than 150ms onsets, so the
true onset/sustain contrast of the RESIDUAL may be larger than any
row yet measured.

Onset numbers were produced by the run but ARE NOT READ, per the
ruling's order of operations: no onset conclusion from a model that
fails its own sustain check. The standing prediction remains unmarked.

---

# Round 9 (2 Sep 2026): three-leg decomposition - the PREDICTION FAILS
# (the bleed is not the mask), LEG 1 does not close, and the remaining
# gap is DEGENERATE between two accounts the instrument cannot separate.

Methodological correction applied throughout: per-instant SIGNED
differences, never medians of absolutes compared. (Now a standing
constraint.)

## Tap extension (approved): effective ratio

PsolaEngine::debugEffRData - d(readPos)/dp = r - bleedApplied, sampled
on a 64-sample grid inside spliceSample (splice jumps are whole
fractional periods, phase-neutral, so this IS the emitted pitch
factor). **BIT-IDENTITY RE-VERIFIED with the extended tap before any
tapped data was read: IDENTICAL, both takes.**

## The three legs (tau6, per-instant signed; NEW take, alto_tenor, D minor)

                       SUSTAINS              ONSETS 150ms         FIRST 50ms
    leg1 T(s)*eff/T(o) +3.72c med, 5.67 |.|  +3.55, 10.15 |.|    -2.18, 16.04 |.|
    leg2 eff vs tgt/f0 +0.62c med, 0.99 |.|  -0.31,  0.89 |.|    -0.48,  0.91 |.|
    leg3 T(o) vs tgt   -0.31c med, 1.74 |.|  -1.38,  4.86 |.|    +1.13,  7.10 |.|

OLD take (chromatic, creak-dominated): leg2 likewise ~0 (1.79c |.|
sustains); leg1 28.75c |.| sustains - both flanks of leg1 swamped by
creak; carries no independent weight.

## THE PREDICTION: **FAIL**, marked explicitly

Predicted: leg2 ~3c on sustains, ~0 in the first 50ms. Measured: leg2
is ~0 EVERYWHERE (0.99c sustains, 0.91c first-50ms, trajectory flat at
0-1c). The slow-feedback account of round 8's gap is dead: slew+bleed
modify the nominal ratio by under 1c at sustains. This is COHERENT with
the shipped design rather than surprising - the 29 Aug shift-gate
ruling gated the bleed off during sustained correction precisely
because a running bleed was measured as convergence tax; the gate is
why there is no mask. The round-8 "engine outperforms its ratio"
framing was an artifact of comparing medians of absolutes - per-instant,
the effective ratio IS the nominal ratio.

## LEG 1 does not close - and what the failure correlates with

Per the ruling: the problem is in the shifter leg, everything upstream
is moot, stop. The ordered correlation report:

    |l1| vs |pitch slope|: r=+0.14 (NEW) / +0.19 (OLD)
        - the displacement/delta-x-slope candidate is NOT the driver
    l1 vs (TS-F0) signed:  r=+0.25 / -0.07
    |l1| vs |TS-F0|:       r=+0.40 / +0.42
        - the gap's MAGNITUDE co-varies with the estimator-vs-tracker
          disagreement: both blow up in the same hard-to-track regions

## The degeneracy, stated honestly (the actual finding)

Per instant, l1 = (TS-F0) + l2 - l3 EXACTLY - four quantities, one
identity, three independent. With l2 ~ 0 and l3 ~ 0 at sustains, leg
1's +3.7c and the apparent estimator error TS-F0 (~+2.8c signed) are
THE SAME NUMBER SEEN TWICE, not two facts. The sustain web (output ON
target, ratio nominal, yet tracker-vs-ring +2.8c) is arithmetically
consistent with only two accounts:
  (A) the shifter's emitted pitch deviates ~3c from f_in * eff; or
  (B) the fine tracker's calibration DIFFERS between raw voice and
      resampled/spliced voice by ~3c (config- and audio-character-
      dependent bias), and the true estimator error is unmeasured by
      everything so far.
No quantity measured in rounds 7-9 separates (A) from (B): every ruler
in the family shares the tracker, and the delta-x-slope caution plus
the calibration web are both instrument phenomena. The correlation
table leans neither way decisively (slope excluded; magnitude
co-variation fits both).

## The discriminating instrument, named for the next ruling, not built

A SYNTHETIC TRUTH-BEARING TAKE: a resynthesized vowel-like signal with
KNOWN f0(t) (onset scoop + vibrato, real-voice-like spectrum) rendered
through the FULL chain. Truth is then known on BOTH sides: T's bias on
raw truth calibrates the tracker; T(output) vs truth * eff closes leg 1
against ground truth rather than against another tracker reading. One
take, every existing tool applies unchanged.

Stopped at the leg-1 clause. No fix, no bar, no flag beyond the
approved debug taps.

---

# Round 10 (2 Sep 2026): synthetic-truth take - SUSPENDED MID-GATE by the
# waveform-discontinuity pivot (Sean: "the start of words is shakey";
# the container found discrete waveform events no pitch metric can see).

State at suspension, so nothing is lost:
- CALIBRATION ARM COMPLETE: **account B EXCLUDED** - b_res-b_raw and
  b_spl-b_raw are <=0.4c at every ratio (+/-10/25/50c), both registers,
  both on the first synthetic and re-confirmed on the hardened one.
  The tracker does NOT read resampled/spliced audio differently. The
  round-9 degeneracy therefore resolves toward account A - with the
  caveat that the offline replica has no FORMANT-PRESERVE stage, so a
  formant-preserve-induced reading shift remains the prime suspect
  inside account A.
- REQUIREMENT-3 GATE: first cut too easy (leg3 onset 2.22c vs real
  4.86c); hardened with stochastic onset wobble (the real take's
  measured 5.22c/hop onset instability was the missing mode) +35ms
  attack + varied scoops; overshot at 18c scale (13.16c), landed at
  7c scale: **onset 6.64c vs real 4.86c, sustain 2.93c vs 1.74c** -
  hardness now within ~1.4x of real on both axes, adequate for
  magnitude-transfer with that factor stated.
- PRIMARY DELIVERABLE: NOT READ (gate iteration was still in flight
  when the pivot landed). The pre-hardening run's numbers are recorded
  in the transcript but are NOT findings.

## STANDING CAUTION (ordered with the pivot), filed beside
## magnitude-is-not-mechanism:

**AN OCCASIONAL DEFECT REQUIRES AN EVENT-LEVEL INSTRUMENT, NOT A BETTER
AGGREGATE.** 12 events across 860 blocks cannot move a median or a p99.
Every metric in rounds 1-10 was an aggregate; Sean's complaint has
always been about specific moments ("at 5 seconds", "at 6 seconds" -
both now matched by container events at 5.57/5.66/5.67 and 6.16s on the
29 Aug bounces).

---

# Round 11 (2 Sep 2026): THE EVENT INSTRUMENT - the container's word-start
# discontinuities reproduce on the CURRENT engine, and the 6.16s event
# sits exactly on a tapped dry->wet seam.

tools/pitch_glitch_events: LTP residual (P from the file's own local
period, g from local normalised correlation), peak/RMS per 10ms block,
excess over aligned source, loud+voiced both sides, threshold 1.5,
events merged within 30ms. Alignment lag reported per file and verified
against unvoiced-source hits (the container's dead Antares column).

## NEW take (24 word starts; alignment clean, 0-2 unvoiced hits per file)

    row                          events  within 150ms of word start
    29 Aug bounce, ign-vib OFF      3        2/3  (67%)
    29 Aug bounce, ign-vib ON      11        2/11 (18%)
    CURRENT tau6, ign-vib OFF       4        4/4  (100%)
    CURRENT tau6, ign-vib ON        7        5/7  (71%)
    ANT_0 (lag +252, 32/736 uv)    13        4/13 (31%)

Container counts (12/30 on the bounces) differ in absolute number from
mine (3/11) - different scoring constants - but the ORDERING and the
event locations agree, which is what matters for an event instrument.

**Sean's timestamps confirmed on the CURRENT engine:** 5.62s (excess
2.2, 65ms after the 5.55 word start) and 6.18s - and the 6.18 event's
tap cross-ref reads: **wet resumes at 6.16s after 22ms of bit-exact
dry, target step 9c** - a dry->wet seam, the DESIGN_SEAM_RESIDUAL
population, caught in the act at the exact second Sean named. Both
events appear in BOTH ignore-vib states. The 2.51s event (excess ~2.0)
is the most stable artifact in the set: present in both 29 Aug bounces
and both current renders at the same timestamp.

**Antares at Retune 0 has 13 events of its own** (strongest in the set:
5.12s, excess 4.42) - the hard snap is not discontinuity-free; its
alignment (+252) carries 32/736 unvoiced hits, short of my suspect
threshold but the weakest alignment in the table.

## OLD take: the ignore-vib ordering INVERTS

    CURRENT OFF: 4 events (1 onset)  |  CURRENT ON: 0 events
    antares.wav fast: 7 events
Three OFF events (2.19/2.47/6.30) also appear in the Antares render -
shared source-roughness moments, not EchoJay-specific.

## JOB 3 - the practical answer for Sean, plainly

On YOUR material (the NEW alto take, the take of the complaint):
**ignore-vib OFF gives fewer glitch events than ON on the current
engine (4 vs 7; 3 vs 11 on your 29 Aug bounces) AND better sustain
tuning. Actionable today.** On the old low-male take the ordering
inverts (0 vs 4), so it is a per-material setting, not a universal.
Cross-referenced in DEFECT_VIBRATO_ON_TUNING_COST.

## JOB 2 - both filings VOIDED as ordered (marked, not deleted)

- DEFECT_GRAIN_EPOCH_UNITY: clearance void (jitter cannot see clicks);
  grain path at unity measures 30 events on sourceNEW / 16 on source4 /
  6 on dry.wav vs 4-7 full-chain; inversion-timestamp cross-check
  partial (probe prints only worst windows; extension needed for 1:1).
- DESIGN_SEAM_RESIDUAL: onset-closure void; seam-vs-event overlap now
  MEASURED - 100% (OFF) / 71% (ON) of current-engine events sit within
  150ms of word starts, and the 6.18s event is tap-confirmed AT a seam.

## For Sean's ear (the fastest validation no ruler substitutes for)

Listen on the current install at: **2.51s, 5.62s, 6.18s, 11.75s** (both
ignore-vib states), plus 0.07-0.31s with ignore-vib ON only. Confirm or
deny; the event list is falsifiable at each named time.

---

# Round 12 (2 Sep 2026): THE MECHANISM RECORDED - the seam step. Ear
# gate delivered. Inversion question answered: the seam is the whole
# story on the production path.

## The mechanism (primary finding of the investigation, per ruling)

Word start goes bit-exact dry (CORRECT - the V/U split prevents PSOLA
on plosives/fricatives). ~22ms later the wet leg resumes with a TARGET
STEP (9c at the 6.16s exemplar) - above the ~8.6c pitch JND. Every word
start with a dry gap therefore carries an audible instantaneous pitch
jump. NOT wobble - a STEP, tau-independent because a seam step contains
no time constant. This accounts for every property of the complaint,
including the tau-invariance that killed nine rounds of target-side
hypotheses.

**DO NOT REACH FOR kSeamFadeMs** (recorded so nobody tries it later):
crossfading two signals 9 cents apart produces 1.5ms of beating
followed by the same step. Pitch perception integrates over tens of ms;
lengthening the fade worsens the beating without shrinking the step.
The seam fade solves WAVEFORM continuity; this is PITCH continuity -
different problems, and the fade was never going to solve this one.

## The fix shape (research doc section 7 item 7, promoted from taste
## knob - the three products disagree about onset direction and amount,
## but all three agree you do not STEP)

The wet leg resumes AT THE DRY LEG'S PITCH - zero correction at the
seam - and ramps to full correction over tens of ms. Prior art: GSnap
"Attack"; Waves Tune RT "Note Transition" (120ms default, separate from
its 15ms Speed).

## The acceptance bar (recorded now; goes in the flag's commit when built)

  ACCEPT:  word-start event fraction falls materially from 100% (OFF) /
           71% (ON) toward Antares' ~31%, NEW take.
  ACCEPT:  no regression in sustain tuning (1.9c) or onset off-grid.
  FALSIFY: the OLD low-male take - ign-vib ON currently has 0 events;
           the ramp must not introduce any there.
  Raw event COUNT is explicitly NOT the bar (ANT_0 has 13 events, more
  than our 4, including the file set's strongest - scattered
  discontinuities read as material roughness; one at EVERY word start
  reads as a broken plugin). Do not optimise count.

## GATE: Sean's ear, BEFORE any build

Delivered ear_gate_excerpts.zip - five 1.5s aligned windows (2.51 /
5.62 / 6.18 / 11.75s + the 0.1-0.3s ON-only cluster), each as source /
EchoJay ign-vib OFF / ON / ANT_0 (its +252 lag applied), 10ms edge
fades so the cut cannot masquerade as the event; the event sits 0.6s
into every clip. From the MEASURED renders, not the install. If he
hears nothing at those points, the events are inaudible and we stop -
that outcome will be reported as readily as the other.

## The inversion question, answered (probe extension committed)

pitch_glitch_events gained "inversions" (grain, census config),
"inversionsfull" (production chain), "inversionssrc" (source baseline);
timestamps printed for 1:1 comparison.

- source4, grain path (the census material): 16 events vs the ungated
  inversion list - EXACT coincidences at 3.23s (sim -0.71, the
  strongest inversion, [ONSET]) and 8.64s, near-miss at 2.06/2.00;
  partial coincidence. Grain-path-only; out of the production band.
- NEW take, PRODUCTION chain: 37 sim<0 windows - against the SOURCE'S
  OWN 33. The voice's consonant discontinuities dominate the inversion
  population. At the four events: 11.71 (-0.41) is render-added and
  coincides; 5.62's are strengthened versions of inversions the source
  already has at 5.47/5.55; 2.51 and 6.18 have none (the 6.18 event's
  driver is the tapped SEAM, which flanking weak sims corroborate).

**VERDICT, in the ruling's words: the seam is the whole story** for the
production-path fix. Inversions are not a coherent second mechanism at
the event locations - one coincidence in four, against a source
baseline of the same order. The grain path's inversion mechanism stays
quarantined in DEFECT_GRAIN_EPOCH_UNITY (already VOID-remeasured).

No build. The seam ramp waits on the ear gate.

---

# Round 13 (2 Sep 2026): EAR GATE PASSED - the build proceeds

**Sean's confirmation, recorded against the 6.18s seam event** (the one
the tap caught in the act - wet resuming at 6.16s after 22ms dry with a
9c target step - at the timestamp he independently named weeks before
it was measured): on the 6.18s clip, "gets worse and worse" across the
three legs - source clean, ignore-vib OFF carries the artifact,
ignore-vib ON worst. That confirms BOTH the event and the
ignore-vibrato ordering by ear, matching the event counts from both
instruments (container 12/30, current engine 4/7).

## THE COMMITTED BAR (this commit precedes the first line of engine
## code; the flag commit repeats it verbatim)

  ACCEPT:  word-start event fraction falls materially from 100% (OFF) /
           71% (ON) toward Antares' ~31%, on the NEW take.
  ACCEPT:  no regression in sustain tuning (1.9c) or onset off-grid.
  FALSIFY: the OLD low-male take's ign-vib ON zero-event state must not
           gain events.
  Raw event COUNT is explicitly not the bar. Do not optimise it.
  IF THE BAR IS MISSED, REVERT. It is not renegotiated.

Build shape: on resuming wet after bit-exact dry, the wet leg starts at
the DRY LEG'S PITCH (zero correction at the seam) and ramps to full
correction over tens of ms. kSeamFadeMs untouched (pitch continuity,
not waveform continuity - reasoning in round 12). Ramp time exposed as
a parameter, not baked (prior art: GSnap Attack; Waves Tune RT Note
Transition 0.1-800ms, 120ms default).

DEFECT_VIBRATO_ON_TUNING_COST: now carries tuning cost + event-count
cost + EAR confirmation; "understood and declined" is no longer
defensible on this material. TO BE RE-OPENED SEPARATELY after the seam
ramp lands - not bundled, so each result stays attributable.

---

# Round 14 (2 Sep 2026): SEAM RAMP v1 BUILT, MEASURED, AND REVERTED BY
# ITS OWN BAR. The bar worked exactly as designed.

Built per the ordered shape: on wet resume after bit-exact dry, ratio
exponent w ramps 0 -> 1 over seamRampMs (w=0 IS the dry leg's pitch,
r^0=1; linear in cents), engine-side at the re-entry site, kSeamFadeMs
untouched, flag setSeamRampMs (0=off). Three re-entry discriminators
iterated BEFORE the bar measurement, each caught by measurement:
  1. Ungated: mid-note blinks retriggered the ramp - ign-vib-ON sustain
     tuning 1.9c -> 2.8c.
  2. Gap-length gate at 60ms (the word-start detector's constant):
     EXCLUDED THE EAR-CONFIRMED EXEMPLAR - the 6.16s word-start seam has
     a 22ms ENGINE-dry gap (detector-unvoiced and engine-dry are
     different clocks). At 15ms: re-admitted ON's 15-25ms blink class.
     GAP LENGTH CANNOT SEPARATE THE POPULATIONS (the ferry lesson again).
  3. Audio-testimony discriminator (the audio-verified-bridging test,
     read-only: periodic-through-the-gap = blink, aperiodic = word
     start, 0.5 threshold, 15ms floor): fixed OFF completely; ON's
     damaging re-entries are GENUINELY aperiodic and pass it.

## The measurements against the committed bar (ramp 60ms, discriminator 3)

  WORD-START FRACTION (NEW):  OFF 100% -> 0% (ZERO events, from 4);
                              ON 71% -> 20% (1/5, from 7)     ** PASS **
  FALSIFY (OLD, ign-vib ON):  0 events -> 0 events            ** PASS **
  SUSTAIN TUNING:  OFF 2.9c -> 2.8c                              PASS
                   ON  1.9c -> 2.8c                           ** MISS **
  ONSET OFF-GRID:  med 4.94 -> 5.04 (noise-level); p75 8.5->10.7,
                   p90 15.4->22.7 - the tails carry the fix's OWN trade
                   (the deliberately-uncorrected first tens of ms).

The ON sustain miss, dissected per-instant (the standing constraint
caught what the medians hid): per-instant delta med +0.03c - NO
systematic regression - but ~45 hops in ONE 150ms region (0.79-0.93s,
the sustain tail of ign-vib ON's turbulent intro note, the same region
whose 36c target steps were event-flagged BEFORE the fix) worsened
+2-3c and crossed the median line, plus a 2-hop +14.8c spike at 4.20s.
Ramp length does not matter (2.7-2.8c at 30/45/60ms) - the damage is
the re-zeroing itself at those specific aperiodic mid-note re-entries,
which pass every discriminator tried because they ARE aperiodic.

## VERDICT: the bar says revert. Reverted.

The engine change was never committed (the bar commit preceded it, per
the ordered sequence); the working tree is restored. The full v1 diff
is preserved at scratchpad/seam_ramp_v1.patch for the next ruling.
No ear set cut (step 6 was conditional on the bar).

## What v1 established for the next shape (findings, not proposals)

  - The fix WORKS on the mechanism: zero word-start events at OFF, the
    ear-confirmed 6.18s exemplar eliminated, falsifier clean.
  - The residual problem is ONE population: ign-vib ON's aperiodic
    mid-note re-entries (15-25ms, intro-turbulence class), where
    re-zeroing a LARGE standing correction (ON holds 30c+ through
    ignored vibrato) costs tuning. Gap length, periodicity, and ramp
    time all fail to separate them from word starts.
  - The un-tried discriminator with a §17.6 pedigree: the SIZE of the
    correction being re-zeroed, or note identity across the gap
    (pre-gap vs post-gap note agreement) - a blink resumes the SAME
    note; a word start usually does not. Not built; not measured.

---

# Round 15 (2 Sep 2026): THE CORRECTED BAR - committed before any code

Two of v1's three misses were errors in the bar as written; correcting a
MALFORMED bar is not renegotiating a FAILED one. Safeguards carried: the
corrected bar can still fail; it is measured FRESH, never re-read off
v1's numbers; and it gains an ear gate no arithmetic can soften.

**STATISTICAL CAUTION FILED** (beside medians-of-absolutes and
magnitude-is-not-mechanism): **A MEDIAN CAN MOVE SHARPLY WHEN A CLUSTER
STEPS ACROSS IT.** ~45 hops in one 150ms region crossed the median
point and dragged a distribution median 0.9c while the paired
per-instant delta median read +0.03c. Never use distribution medians
for a localized effect.

## THE CORRECTED BAR (verbatim)

  ACCEPT: word-start fraction falls materially from 100% (OFF) / 71%
          (ON) toward Antares' ~31%, NEW take.
  ACCEPT: paired per-instant sustain tuning delta median <= +0.15c in
          BOTH ignore-vib configs, AND no contiguous region larger than
          50ms worsening by more than 2c.
  ACCEPT: onset off-grid MEDIAN not regressed. Tails reported, not
          gated (the p75/p90 regression IS the ordered fix shape).
  FALSIFY: OLD low-male take, ign-vib ON, must stay at 0 events.
  EAR GATE: Sean must not hear damage in ON's turbulent intro note
          region. Hard gate; cannot be argued past.
  Raw event count remains explicitly not the bar.

V2 = v1 + the CORRECTION-SIZE DISCRIMINATOR (suppress the re-zero when
the standing correction exceeds a threshold DERIVED from the measured
populations; note-identity-across-the-gap held in reserve). Dead ends
not to be retried: ramp length, gap length, ungated.

Reconciliation ordered before v2's ON numbers are trusted: the
container had OFF tuning better than ON (29 Aug bounces); this record
has ON better at sustains (current engine). Which ordering is real, on
matched renders, decided below.

## Round 15 results: v2 measured FRESH against the corrected bar - ALL
## MEASURED LEGS PASS; the ear gate remains

Both ordered discriminators were REFUTED BY POPULATION DATA before
building them (tap-derived tables, ON config, 55 re-entries):
  - CORRECTION SIZE does not separate: word-start re-entries carry
    0.6-251c standing values (132c AT THE EXEMPLAR - the stale pre-gap
    target against the new note's f0), fully overlapping mid-note's
    0.6-1096c. The premise inverted: word starts hold the LARGEST
    stale values, which the corrector re-anchors within ~25ms anyway.
  - NOTE IDENTITY does not separate: this singer starts words on the
    held pitch (exemplar noteDelta 2.4c - same note); both populations
    cluster at 0-15c. The "word start usually changes note" premise is
    false on this material.
  - Bonus classifier finding: many "mid-note" re-entries sit ~22ms
    BEFORE the detector's word start (engine voices first) - they ARE
    word starts, correctly ramped by the audio-testimony discriminator.
V2 therefore ships v1's mechanism unchanged (audio-testimony
discriminator, 15ms floor, 0.5 periodicity); the residual v1 "damage"
is adjudicated by the corrected bar's own clauses below.

    LEG 1  word-start fraction: OFF 100% -> 0% (ZERO events), ON 71% ->
           20%                                                    PASS
    LEG 2  paired sustain delta med: ON +0.032c, OFF -0.009c (cap
           +0.15c); longest >2c contiguous worsening: ON 29ms, OFF 8ms
           (cap 50ms)                                             PASS
    LEG 3  onset off-grid, PAIRED per-instant (the bar restated all
           tuning legs as paired): ON -0.066c, OFF -0.013c - not
           regressed                                              PASS
           Tails, informational: unpaired p75 8.5->10.7 / 8.7->11.0,
           p90 15.4->22.7 / 18.4->25.7. Population note: v2 emits
           79-82 MORE trackable early-window hops (dry-like openings
           track better); the unpaired medians' +0.10/+0.39c were
           entirely this population change - the cluster-across-median
           caution's first live catch.
    LEG 4  OLD take, ign-vib ON: 0 events -> 0 events            PASS
    LEG 5  EAR GATE: pending Sean - hard, cannot be argued past.

Parameter: seam_attack_ms (0-150, DEFAULT 0 until the ear gate passes,
then 60). Default derivation: 60 = geometric midpoint of the ~30ms
pitch-integration floor and Waves Tune RT's field-proven 120ms Note
Transition default, AND the measured event-elimination point on this
material. Plugin builds clean.

# Round 16 (3 Sep 2026): ear gate PASSED; seam_attack_ms default 0 -> 60

LEG 5 of the corrected bar closed: Sean heard ON's intro region at 60ms
and reported no audible damage. All five legs now pass. The default flips
to 60 in the schema (Source/EedPitchProcessor.cpp, kSeamAttackMs) with the
derivation unchanged: geometric midpoint of the ~30ms pitch-integration
floor and Waves Tune RT's 120ms Note Transition, and the measured
event-elimination point on this material.

## WHAT THIS FIXES, AND WHAT IT DOES NOT - the record, stated plainly

**It fixes the SEAM STEP** (round 12's primary finding): the 9c
instantaneous target step when the wet path resumed after a bit-exact-dry
consonant, tau-independent, above the 8.6c JND - the whole of Sean's
"word-start glitch" complaint on the production path. Measured: word-start
glitch events OFF 100% -> 0% (zero events), ON 71% -> 20%, OLD-take
falsifier 0 -> 0, paired sustain and onset medians unmoved.

**It does NOT touch the ONSET-ACCURACY GAP against Antares** (round 4's
primary finding, still open). At matched Retune 0, Antares corrects onsets
to med 1.94c off-grid / all-voiced <5c 76.3%; our current-engine best is
4.95c / 59.9% (2.6x looser). Seam attack moves in the OTHER direction at
the tails BY DESIGN - it deliberately leaves the first tens of ms of each
word start uncorrected (ramp from the dry pitch), which is why the onset
p90 went 15.4 -> 22.7c (ON) / 18.4 -> 25.7c (OFF) in the v2 measurement.
That is the fix's own shape, accepted by the bar as informational, and it
means: anyone reading "onset events fixed" as "onsets fixed" is wrong.
Section 4 of ONSET_SHAKINESS_RESEARCH.md (bypass-on-low-confidence,
narrow-band tracker, detector-side rate limit) remains LIVE and untouched
by this work. Closing the Antares onset gap is a separate pass with its
own bar.

## VALIDATION CAVEAT (carried on the default, until discharged)

The 60ms default is validated on exactly two pieces of material: the
standing NEW take (sourceNEW, one singer, one key, one already-corrected
phrase) as the measured bar, and the OLD low-male take as the falsifier.
Nothing else has been rendered through the ramp. REFERENCE_SET.md forbids
substituting a take without a ruling, and no third take exists on disk, so
**broader material validation is OWED, not done**: at minimum a different
singer, a breathier/aspirated-consonant take (the population the audio-
testimony discriminator was built for), and a fast-syllable take where
60ms is a large fraction of the vowel. Until that lands the default is
"flipped", not "shipped". Also owed: a re-measure of the OLD take's
ign-vib OFF events (the falsifier ran ON only).

## Re-opened by this round (its own item, NOT bundled with seam attack)

DEFECT_VIBRATO_ON_TUNING_COST.md is re-opened as a standalone defect - see
its 3 Sep 2026 section. Seam attack does not fix ON's residual (20% word-
start fraction remains vs OFF's 0%), and the tuning cost cross-referenced
there has now survived the ear pass its "understood and declined" status
was conditioned on.

# Round 17 (3 Sep 2026): the slow end of the retune dial - see SLOW_END_RECORD.md

Seam attack self-check: NOT our fix (60ms is calmer than 0 at tau 150 on
every column). Sweep and attribution filed there; the note-boundary snap is
the bucket that scales with tau; the detector leads glides by ~30ms;
event lists are analysis-grid-phase realisations (64 samples flips events)
and must not be compared hop-for-hop across bounces. Transport-start effect
measured: one note, bounded. Nothing built.
