# DECISION DOCUMENT: unifying on the shift path (2 Sep 2026 — investigation only, nothing built)

## Why this question exists

The shift path's boundary behaviour measures 3.7–8.3c applied-shift
discontinuity at EVERY τ and voice; the legacy target/f0 path measures
42c at τ20 rising to 181c at τ400. Every defect of the boundary-snap
class — the snap itself, the ferry mis-investigation, the env5 candidate
and its regression, the median destination, pending-forget, the corridor
lineage — lives on the legacy path. Routing everything through the shift
path retires the class. This document answers the three ruled questions
before any line moves.

## Q1 — the "+7 clicks" finding: reproduced, dissected, and NOT inherent

Reproduced 2 Sep 2026 (debugForceShiftPath, chromatic hard-match, k=0):
clicks 2 → 11 (the recorded +7, slightly worse on this material), AND a
second cost the comment never recorded: under-correction 10.5 → 13.1c
(fails the cents bar). HNR/flux/HF actually IMPROVE (+0.09dB, +2.0%).

The mechanism, from the code: shiftCents_ = shiftSm_ + (k−1)·osc. The
slow term is pole-smoothed and boundary-shielded — it is why the shift
path is clean. The FAST term (k−1)·osc is hop-sampled corrector-side and
carries the documented phase caveat: it is applied to audio emitted
37–55ms later (~a third of a 6Hz cycle), so at k=0 the per-hop −osc
staircase steps the engine ratio at hop boundaries (the clicks) and
mis-phases the flattening (the under-correction — wobble incompletely
cancelled measures as distance from the note).

**Verdict: a property of WHERE the fast term is sampled, not of the
shift path as a concept.** The legacy path avoids it only because its
ratio target/f0Here reads the RING's lag-compensated per-sample f0 — the
fast information applied at the audio's own time. The candidate fix is a
HYBRID: slow component via the (shipped, shielded) shift term; fast
component applied engine-side from the ring (ratio scaled by
(f0ring/slow-of-ring)^(k−1), phase-correct by construction). This is a
designable DSP change, not a removal of a limitation baked into the
architecture.

## Q2 — what the legacy path still provides, named

1. **k≠100 fast semantics** (hard k=0 flatten, tuned k=40, exaggerate
   k=200, custom): expressible on the shift path structurally (the term
   exists) but UNSHIPPABLE until the fast term is ring-aligned (Q1).
   This is the whole blocker.
2. **Fixed-target mode (target_hz, the P1 diagnostic knob)**: pure
   target/f0 semantics. Expressible as shift = targetCents − inCents per
   hop; needs the same ring alignment for phase-correct tracking.
3. **Per-sample vibrato tracking within a hop**: legacy gets it free
   from the ring read; the shift path quantises at hops. Covered by the
   same hybrid.
4. Nothing else: scale/key/flex/humanize/transpose/reference are
   corrector-side and path-agnostic; formant modes are engine-side;
   F0JumpGate protects the ring for both.

## Q3 — migration cost, in defects-at-risk terms

Re-validation required:
- The chromatic six-metric hard-match gate (hard flips path; currently
  FAILS forced — clicks 11, cents 13.1 — so the migration is gated on
  the Q1 hybrid landing first and passing it).
- pitch_click_test (the acapella click-density gate).
- Field panels at hard, both voices, both takes (re-baseline: the
  legacy rows of every table in §17.4/§17.5 become shift rows).
- §17.5 register: every constant re-derives — notably the 30c env5 gate
  (its bracket [8.3, 100] came from the PATH SPLIT; unification
  collapses it, which is precisely the class retirement).
- Boundary probe, constshift probe, ab mirror: all pass kNoShift/legacy
  explicitly and need the new routing (mechanical).
Pinned suites blast radius: psola_engine_test keeps the kNoShift API
(engine semantics unchanged — the ENGINE keeps both ratio sources;
routing changes corrector-side); pitch_mode_test unaffected (mode
determinism is param-level); corrector suite: the natvib monotonicity
and slow-shift tests already point at input+shift and survive.

What gets RETIRED if it lands: envExp 5 and its four-gate candidate, the
median destination, pending-forget, the corridor machinery, the
carried-offset gate — the entire §17.6 worked-example lineage becomes
dead code behind flags, removable. The snap class becomes unreachable
rather than mitigated.

## Recommendation shape (for ruling, not built)

Build the ring-aligned fast term as an engine-side experiment behind a
flag; acceptance = the chromatic six-metric gate at hard ON the shift
path (clicks ≤ Antares+1, cents within margin), then the standard stop
panels. If it passes, unify routing and run the retirement list. If the
fast term cannot pass the click gate ring-aligned, the answer is "the
legacy path stays, env5's four-gate path is the standing mitigation" -
and this document records why, with numbers.
