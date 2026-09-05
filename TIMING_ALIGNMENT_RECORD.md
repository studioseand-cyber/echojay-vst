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
