# TIMING ALIGNMENT - the discipline, the audit, the fix shape, THE BAR (5 Sep 2026, round 28)

**THE DISCIPLINE (ruled):** EVERY QUANTITY THAT MEETS IN THE RATIO CARRIES
THE SAME AUDIO TIMESTAMP, ENFORCED STRUCTURALLY RATHER THAN REMEMBERED PER
CALL SITE. Three independent findings of one class: round 18's denominator
half-lag (f0Here 6.5 ms alto / 10 ms low male behind the audio); round 27's
numerator (target stamped at the hop, applied at the read pointer: emitted
pitch 11-13 / 18-20 ms ahead of the audio at depth 0); and the assumed third
site, found by this audit (§2, item 7).

## 1. Where the skew comes from, in samples (alto_tenor at 48 kHz)

  raw published f0 at hop P describes audio at P - 1469 (30.6 ms)   [lead probe]
  f0 ring write:   at = P - pitchLag (1180)  -> f0Here(p) describes p - 289 (6.0 ms)   [denominator lag]
  target/shift:    scalar per call, applied from base = write - n - latency (1800):
                   the hop-P target (describing P - 1469) is applied to audio at
                   ~P - 1800 (+ the read pointer's drift)  -> ~5-7 ms LEAD    [numerator lead]
  ratio tgt/f0Here therefore joins two instants ~11 ms apart (alto), ~18-20 (low male).
  Steady notes: 0.00c. Every glide and every vibrato cycle: error = skew x slope.

## 2. THE STAMPING AUDIT - every quantity that reaches the ratio (EedPsolaEngine.h)

  #  quantity            stamped how                                        status
  1  f0Here              f0_ ring at write-n+i-lag; read at rp             RING, lag short by (tauMax-tau)/2 + ~1.2 hops  (round 18)
  2  tgt (curTarget_)    per-call scalar = this call's targetHz; read at rp  NOT RING-STAMPED: the hop's clock, applied at the read clock  (round 27)
  3  curShift_           per-call scalar = this call's shiftCents            NOT RING-STAMPED, same as 2; the shift path has no denominator so it carries the numerator lead only
  4  fastFactor          f0Here / slowHere, both from rings at rp            CO-TIMED (the ring-aligned fast term, by design)
  5  spliceT_/spliceTf_  fs / f0Here at rp                                   CO-TIMED with 1
  6  grain-path ratio    target / f0 in advanceSynthesis (its own comment: "legacy: crosses the latency")   NOT RING-STAMPED (same as 2)
  7  F0JumpGate testimony  inputPeriodicity(ev.inputPos, tOld/tNew): reads audio AT the hop position while vetting an estimate that describes inputPos - 30.6 ms   THE ASSUMED THIRD SITE: a 30 ms skew between the audio tested and the estimate vetted (decides what f0 enters the ratio; impact unmeasured - it may accept jumps early, which could be benign or helpful; a bar leg measures it)
  8  audio-verified bridging  inputPeriodicity(q+L, L) around the ring write position   CO-TIMED with the f0 ring write
  9  seam-ramp discriminator  inputPeriodicity(mid, T0) at the gap's audio position; T0 = fs/g from the hop's f0   audio position correct; period from the hop clock (minor: a period, not a pitch instant)
  10 vibNow_, transpose  added to targetCents_ at the hop                    non-audio, inherit 2's stamp - correct once 2 is ring-stamped
  11 emitDry decision    target <= 0 per call                                 per-call; seams themselves are ring-governed (f0=0 in the ring), correct

## 3. THE FIX SHAPE (behind flags, each measured alone, then together)

  FLAG A - CO-TIMED TARGET RING: tgt_ and sh_ rings sized like f0_, written
  at the SAME `at` positions as f0 (this call's targetHz / shiftCents), read
  at the read pointer (rp) beside f0At(rp) in the splice ratio and at the
  epoch in the grain ratio. Numerator and denominator become one instant by
  construction. curTarget_ stays only for the emitDry decision.
  FLAG B - PER-HOP LAG (round 18): lag_h = W/2 + tauMax + 2 - tau_h/2 + 2 hop,
  tau_h = fs/track, replacing the constant frameLen/2 + hop; applied to all
  three rings. PitchEngine exposes W, tauMax, hop per voice type.
  FLAG C - GATE TESTIMONY BACK-DATING (item 7): inputPeriodicity at
  inputPos - lag instead of inputPos. Measured alone; may be declined.

## 4. THE BAR (committed before the first line of engine code - this commit)

  1. FLAGS OFF: bit-identical to the shipped build on sourceNEW at retune 44
     and 6, both vibrato modes (cmp).
  2. SYNTHETIC TRUTH (tools/pitch_lead_probe): with A, the emitted-at-depth-0
     skew falls from +11..+13 ms (alto) / +18..+20 (low) to within +-1.5 ms
     of the f0Here residual (i.e. the two clocks are one); with A+B, both
     the shifter f0Here lag and the emitted skew are within +-1.0 ms at
     110/165/250/330 Hz alto and 80/110/160 Hz low male, rates 0.25-2 c/ms.
     Steady-note error unchanged (<= 0.15c).
  3. THE RIPPLE LEG (ruled): a synthetic steady note with 40c / 6 Hz vibrato
     at depth 0 (mode 2): emitted-minus-truth ripple amplitude at the
     vibrato rate BEFORE (predicted ~8-10c alto) and AFTER (target <= 1c).
     If it does not appear before, that is reported as weakening the skew
     account. Plus sourceNEW: activity restricted to hops with |slope| >
     0.5 c/ms (vibrato and glides), before/after, at depth 1 and depth 0.
  4. sourceNEW, hard, ign OFF, seam 60, retune 44 (Sean's point) and 6:
     depth 0 (mode 2) activity median <= 0.5c and improve-rate == the unity
     control's (transparency reachable); depth 1: DETECT-bucket >25c count
     falls from 54 (tau 44) / 38 (tau 6), total >25c events do not rise.
  5. Paired per-instant sustain delta median <= +0.15c, no contiguous >50ms
     region worsening >2c, both vibrato modes; onset off-grid MEDIAN not
     regressed, paired; OLD-take falsifier: ign ON word-start events 0 -> 0.
  6. THE CANCELLATION CLAUSE (intact from round 18): the lag REDUCES the
     3.2s chase excursion today; if the 3.05/3.2s event peaks worsen while
     legs 1-5 pass, that is NOT a failed fix and does NOT revert - the pass
     widens to the chase. Any other regression reverts as normal.
  7. FLAG C measured alone: gate rejected-hop and confirmed-jump counts,
     onset off-grid median paired, and the >25c events on sourceNEW; kept
     only if it moves nothing for the worse; declined otherwise, with its
     numbers.
  8. THE STAMPING AUDIT REPORTED: §2 re-read after the build, every row's
     status updated, any new quantity that reaches the ratio listed.
  9. Sean's ear: retune 44, ign OFF, before/after, one file; the 3.05/3.2s
     boundaries named.

# Round 28 BUILD (5 Sep 2026): flags A, B, D built behind flags (default OFF, bit-identical off); measured against the bar

Code: EedPsolaEngine.h - tgt_/sh_ rings written at the f0 ring's positions
(A, setCoTimedTarget), read at the read pointer in the splice ratio and at
the epoch in the grain ratio; per-hop lag from the detector's window
geometry (B, setPerHopLag; PitchEngine::lagModelFor) with CONTIGUOUS ring
writes (a varying lag left gaps = unvoiced = spurious seams: unvoiced
output hops 156 -> 287 before contiguity, 189 after); explicit target
lookahead (D, setTargetLookahead, investigation). Tools: PA_COTIMED,
PA_PERHOP, PA_LOOKAHEAD, PA_ENVEXP, PA_FORCESHIFT, PA_SLOPE_MIN. Logs in
tools/pitch_activity/logs_2026-09-03/timing_*.txt.

## The legs

  1 identity (flags off)                       PASS  cmp-identical, retune 44 and 6
  2 synthetic truth                            A: PASS - emitted skew +11..+13 ms -> 0.00 ms at every rate, both voices
                                               B: MISS by <= 1.1 ms - f0Here residual 6.5 -> 1.2-1.3 ms (alto), 10 -> 1.8-2.1 (low male);
                                                  the +2 hop term is ~0.45 hop short (alto), ~0.75 (low male); round 18's unidentified constant
  3 THE RIPPLE (ruled leg)                     APPEARED AND WAS REMOVED: 40c p-p / 6 Hz vibrato at depth 0 - emitted error
                                               6.34c rms / 10.47c peak (alto), 9.34 / 16.21 (low male) BEFORE; 0.00 / 0.00 with A.
                                               The prediction (~8-10c) held; the skew account is strengthened.
  4 sourceNEW depth 0 (transparency)           PASS  0.00c median/p90/mean, improve-rate == unity (A+B, both retunes)
    depth 1 DETECT >25c count                  PASS  63 -> 9 (retune 44), 38 -> 0-1 (tau 6)
    depth 1 total >25c does not rise           FAIL  retune 44: 8.8% -> 12.9% (B), 18.2% (A+B); tau 6: 3.8 -> 1.7% (B), 4.0% (A+B); tau 150: 16.0 -> 20.0 / 23.9
  5 sustain tuning (sustune, retune 44)        PASS  off-grid 2.3 -> 1.6c, same-semi 98.8 -> 99.4%, improve 81.1 -> 79.5% (paired per-instant not run)
    OLD-take falsifier (ign ON, tau 6)         PASS  word-start events 0 -> 0
  6 THE CANCELLATION CLAUSE                    FIRED, take-wide: the 3.2s peak -61c -> -78c (A+B); activity median at retune 44
                                               5.04 -> 7.24c. NOT a failed fix, NOT a revert (written before the build): the
                                               accidental ~7 ms target lead was applying note decisions early and hiding part of
                                               the chase; co-timing shows the chase in full. The pass WIDENS to the chase.
  7 flag C (gate testimony)                    NOT RUN this round
  8 stamping audit                             UPDATED: rows 2, 3, 6 now RING-STAMPED (A); row 1 residual 1.2-2.1 ms (B); row 7 untested
  9 Sean's ear                                 PENDING (nothing installed; flags default OFF)

## The widened pass, measured this round (the chase under co-timing)

  arm (retune 44 / tau 6 / tau 150, ign OFF, depth 1)    activity median      >25c share
  shipped                                                 5.04 / 3.95 / 6.25   8.8 / 3.8 / 16.0
  B only                                                  6.15 / 4.13 / 7.56   12.9 / 1.7 / 20.0
  A+B                                                     7.24 / 4.54 / 8.21   18.2 / 4.0 / 23.9
  envExp 5 only (no flags)                                4.08 / 4.03 / 5.12   4.5 / 3.7 / 13.5   <- helps alone, at every tau
  A+B + envExp 5                                          6.60 / 5.00 / 7.94   16.5 / 5.7 / 21.7   <- does not recover what A exposes
  B + envExp 5                                            5.34 / 4.72 / 6.58   10.9 / 3.9 / 18.5
  forced SHIFT PATH, shipped                              6.96 / 6.24 / 7.13   17.9 / 14.6 / 18.2  <- worse than legacy at hard already
  forced SHIFT PATH, A+B                                  7.77 / 7.26 / 7.93   22.7 / 18.1 / 23.4
  A+B + lookahead 2 / 4 / 6 ms (retune 44)                7.03 / 6.69 / 6.45   16.8 / 15.3 / 13.2  <- monotone, bounded
  A+B + lookahead 7.5 / 9 ms                              16.5 / 18.3          36 / 42             <- past the latency budget (writes fall behind
                                                                                                        emitted_): the budget is ~6-7 ms at 256-sample blocks
  lookahead's cost at depth 0 (transparency)              L=4: 1.13c median; L=7.5: 9.81c

## What this says, plainly

  - Co-timing (A) is CORRECT: the ratio's two clocks are one, the ripple is
    gone, depth 0 is exactly transparent, and the DETECT bucket is empty.
  - B is nearly right; its last ~1 ms per voice type is the round-18
    unidentified constant and is not worth fitting blind.
  - The shipped engine was borrowing ~7 ms of target lookahead by accident
    and using it to soften the chase. An explicit lookahead can keep at
    most ~6 ms of that (the latency budget) and buys back about half the
    activity rise - at a linear cost to depth-0 transparency (1.1c per
    4 ms). The two goals - a transparent slow end and a softened chase -
    pull on the same knob, which is why the bar's leg 6 exists.
  - The chase itself is the remaining defect, now un-hidden. envExp 5
    helps every tau on the shipped engine (retune 44: 5.04 -> 4.08c, >25c
    8.8 -> 4.5%) but is a partial answer under co-timing. The forced shift
    path is not the answer at hard.

## For ruling (nothing installed; flags default OFF; bar legs recorded as above)
  (a) Accept A (and B at <= 1.3 ms) as the ratio's correct timing, on the
      transparency, ripple and DETECT evidence, and treat the depth-1
      activity rise as the chase honestly exposed - the leg-6 outcome.
  (b) The widened pass then needs the chase addressed WITH co-timing on:
      candidates measured here are envExp 5 (partial), explicit lookahead
      <= 6 ms (partial, costs transparency), the post-envelope depth
      control (the user's mitigation, not a fix), and - not measured - a
      larger shifter latency to buy more lookahead (a product trade), or
      a shorter confirm window now that the target clock is honest.
  (c) Whether to run flag C (gate testimony back-dating) before deciding.

# Round 29 (5 Sep 2026): the clause honoured; the ripple as exemplar; MEASUREMENT CLOCK vs DECISION CLOCK separated and measured; flag C declined; the constants enumerated

## Recorded as ruled
  - THE CANCELLATION CLAUSE WAS WRITTEN BEFORE THE MEASUREMENT (round 18,
    restated in this file's bar) precisely so that this outcome - activity
    up, the 3.2s peak deeper - could not be argued either way afterwards.
    It fired; it is honoured; the pass widens. Nothing reverts.
  - THE RIPPLE is the exemplar of the PREDICT-MEASURE-REMOVE standard:
    predicted from the mechanism in advance (~8-10c at vibrato rate in
    every setting), measured (6.3c rms / 10.5c peak alto; 9.3 / 16.2 low
    male), then removed to 0.00 by the fix that the mechanism named.

## Flag E: SEPARATE THE MEASUREMENT CLOCK FROM THE DECISION CLOCK - built and measured

Design: the ring carries the DECISION - the correction in cents relative
to the hop's own f0, dec = 1200*log2(target/f0_hop) - stamped
decLookahead_ samples early. At the read pointer the target is
f0Here * 2^(dec/1200): f0Here cancels in the ratio (trivially co-timed),
only the decision is advanced, and depth 0 (dec = 0) is identity at any
lookahead. setDecisionLookahead; PA_DECLOOK.

    A+B + decision lookahead (retune 44, ign OFF, depth 1)   activity med / >25c   off-grid / <5c / improve
    0 ms                                                      7.24c / 18.2%         5.30 / 49.1% / 55.8%
    2 / 4 / 6 ms                                              7.23 / 7.10 / 7.16c;  17.6 / 16.8 / 15.8%
    6 ms                                                                            4.86 / 50.7% / 57.8%   <- BETTER tuning than shipped (5.32 / 48.3 / 58.2)
    7 ms                                                      6.47c / 13.5%         5.89 / 46.1% / 54.6%   <- correction starting to be lost
    8 ms                                                      4.00c /  7.6%         6.58 / 42.0% / 55.2%   <- ARTEFACT: writes behind emitted_ skipped
    10 ms                                                     0.00c /  0.3%         6.60 / 39.9% / 11.9%   <- NO CORRECTION AT ALL, reads as "transparent"
    depth 0 at 6 ms                                           0.00c                 (transparency preserved - the ruling's hypothesis CONFIRMED)
    tau 6 at 6 ms: 4.41c / 3.6% (shipped 3.95 / 3.8);  tau 150 at 6 ms: 8.14c / 21.2% (shipped 6.25 / 16.0)

WHAT IT SAYS: the decision can be advanced while the pairing stays co-
timed, at ZERO transparency cost - the separation is real. Its reach is
bounded by the same latency budget as flag D (writes behind the emitted
pointer are skipped: ~6 ms valid at 256-sample blocks, 7 ms already
partial), so it recovers only part of the take-wide rise: at retune 44
the >25c share 18.2 -> 15.8% (shipped 8.8), p90 39.6 -> 35.0 (shipped
23.1). It IMPROVES TUNING beyond shipped. The remainder is the chase,
honestly shown, at the confirm window's length. The two product trades
(larger shifter latency to extend the budget; a shorter confirm window
now that the target clock is honest) are therefore NOT made unnecessary
- they are what is left, and they are now cleanly separable from the
timing fix.

CAUTION FILED (beside the positive-control method note): A TRANSPARENCY
READING NEEDS A CORRECTION CONTROL. 0.00c activity is "no correction
applied" as readily as "correction applied and cancelled"; the L=10
arm read as perfect transparency with an improve-rate of 11.9%. Every
activity claim in this record from here on carries its improve-rate.

## Flag C (gate testimony back-dating, row 7): RUN, and DECLINED with its numbers

    gate: rejected hops 21 -> 19, confirmed jumps 4 -> 5 (one more accepted jump)
    activity, C alone:   retune 44 5.04 -> 5.29c (>25c 8.8 -> 10.5%); tau 6 3.95 -> 3.93; tau 150 6.25 -> 7.29 (16.0 -> 20.0%)
    with A+B+E7:         retune 44 6.47 -> 6.65; tau 6 4.05 -> 4.11; tau 150 7.69 -> 8.81 (19.5 -> 23.3%)
The third site is real (the audio tested and the estimate vetted are 30 ms
apart) and correcting it moves everything for the worse: the gate's
early look at the NEW note's audio is, like the accidental target lead,
doing useful work - it vets a jump against the audio that has already
moved. Row 7's status: KNOWN, DELIBERATELY KEPT, with these numbers. If
the gate is ever redesigned, the vetting position is a parameter, not a
timestamp to be "fixed".

## Audit row status after round 29
  1 f0Here: RING, per-hop lag (B), residual 1.2-2.1 ms      2/3/6 target, shift, grain ratio: RING-STAMPED (A); decision advanced (E)
  4/5/8: co-timed (unchanged)   7 gate testimony: KNOWN, KEPT (C declined)   9 seam discriminator period: hop clock (minor, unchanged)
  10 vibNow/transpose: ride the decision ring under E (non-audio; correct)    11 emitDry: per call (unchanged)

## STANDING RULE (ruled) and THE CONSTANTS, ENUMERATED - not re-derived

  A CONSTANT CALIBRATED AGAINST A DEFECTIVE MECHANISM MUST BE RE-DERIVED
  ONCE THE DEFECT IS FIXED. Every constant below was tuned against a
  clock carrying an 11-20 ms skew (and, it now turns out, a 7 ms
  accidental decision lead). Enumerated first; the order of re-derivation
  is a ruling.

  constant (value)                     where            calibrated against                                       under co-timing
  kRetuneFloorMs (6 ms)                corrector        §17.3: Antares' 0 vs our 4-6 ms, output timing           SUSPECT - §17.3 already known to measure a non-discriminating property; and the floor's audible meaning was the skew's
  kNoteConfirmMs (25 ms)               corrector        the note-change chase / snap as heard and measured        SUSPECT (HIGH) - tuned with 7 ms of the chase hidden; the remaining depth-1 rise IS this window
  the 150 ms cap (kMaxRetuneMs)        corrector/§17.7  §17.4 U-curve, roughness/within-3c, all measured w/ skew  SUSPECT - the knee (40-80) and the cliff were read on a skewed clock; re-measure the sweep with A+B+E
  seam_attack_ms default (60)          shifter/schema   word-start events at seam 0 vs 60, tail p90s (round 15)   SUSPECT - measured under skew; re-run the corrected bar's five legs with A+B+E before "shipped"
  envExp 5's 30c applied-shift gate    corrector/§17.5  geometric midpoint of 8.3c shielded ceiling and 100c      SUSPECT - both bracket ends were applied-discontinuity measurements taken with the skew
  kPendForgetMs (30), kGapForgetMs (30) corrector/gate  hop-clock evidence staleness                              LOW - hop clock only; but their audio-testimony inputs are row 7 (kept)
  gate kConfirmMs (50), kBigJumpCents  gate             the user-stated 40-60 ms window; octave lattice           LOW/MEDIUM - testimony position kept deliberately (C declined)
  kGapIsNoteChangeMs (200)             corrector        gap = note change, hop clock                              LOW
  kVibratoSmoothMs (140)               corrector        slow-track smoothing; fix (d) re-seed measurements        MEDIUM - the re-seed windows were measured on output tuning with the skew
  kSustainMs (180), kStableCents (60)  corrector        sustain judgement, hop clock                              LOW
  kNoteChangeCents (90), kCorridorSlackC (15)  corrector  detection thresholds in cents                           LOW
  3c drift-bleed cap, kBleedMaxRate    shifter          phase drift bound (§17.5 item 3)                          LOW - phase, not pitch timing
  kSeamFadeMs (1.5)                    shifter          waveform crossfade at seams                               LOW - engine clock, ring-governed seams
  seam ramp 15 ms floor / 0.5 periodicity  shifter      the census/bridge convention                              MEDIUM - period taken from the hop clock (row 9)
  kSpliceBandSt (2.5)                  shifter          splice-vs-grain method switch on |ratio|                  LOW
  kLookaheadDefault (3 periods, 1-4)   shifter          the synthesis lookahead -> the LATENCY BUDGET             NOW LOAD-BEARING - it bounds flags D/E at ~6 ms; the product trade
  pitchLagFor = frameLen/2 + hop       detector         the design's own back-dating model                        THE DEFECT ITSELF - superseded by flag B's per-hop model
  kWindowPeriods (2.5), kHopSeconds    detector         YIN window geometry                                       NOT SUSPECT - but now inputs to the lag model (B)
  kContinuityMaxS, kHistoryClearS      detector         detector memory                                           LOW
