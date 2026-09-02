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
