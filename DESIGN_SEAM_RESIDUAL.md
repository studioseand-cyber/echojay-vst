# DESIGN PROPERTY: the per-boundary pitch step under the sacred-dry contract

**Status:** ruled a design property, not a bug (2026-08-29, closing the
waveform-continuity investigation). This document exists so nobody re-derives
the dead ends; every claim below is a measurement on the record.

## The property

EchoJay's correction contract keeps unvoiced content bit-exact dry. On real
voice, every boundary between a corrected voiced span and dry material
carries a genuine pitch step (corrected pitch ↔ source pitch) plus a bounded
drift-remainder ease. A cycle-similarity ruler charges each boundary; on the
standing NEW reference the detector emits ~8 span boundaries per second of
voiced material (tools/pitch_span_census), so the residual "roughness"
measure is dominated by boundary count, not by any implementation defect.

## The evidence chain — every lever measured, in order

1. **The engine equals its own ideal.** A glitch-proof per-span phase-aligned
   resample on the same gated track scores identically, window for window
   (constant-shift probe: 664/36/1 vs 664/36/1 at +5c). Nothing implementable
   beats the floor without changing the contract.
2. **Drift-bleed** (≤3c, τ=100ms, to zero): SHIPPED. Cut +5c breaks 36→15,
   beat the old floor (it changes the contract's drift term), tuning gates
   improved (under-correction 10.4c vs Antares 11.1c). On the field ruler it
   is the difference between Sean's pre-bleed bounce (81 spans / 0.35s vs
   Antares) and the current engine (51 / 0.19s) on identical material.
3. **Drift-carry across short gaps:** monotonically WORSE (15→30→123 across
   the threshold sweep). The discharge cost was never the reset: re-anchoring
   makes the entry fade free; carrying gives both fades an offset. Capability
   retained off (`setDriftCarryMs`), commit e410971.
4. **Bleed-to-nearest-multiple:** measured IDENTICAL (policies coincide
   wherever |drift| < T/2, which is everywhere that occurs). Reverted;
   latch design in commit 1151b4b.
5. **Audio-verified bridging** (θ swept 0.5–0.8, 100ms cap, measured f0):
   confirmed the boundary model on the vs-ideal column (worse-spans halved,
   16→8) but did NOT move the binding field number — rough spans vs Antares
   51→50 (vib off), 56→57 (vib on), same take, same ruler, zero inversions
   either way. Died by the pre-declared bar, commit dd44350. Capability
   retained off (`setF0Bridge`).
6. **Fade length** (1.5/10/25ms, post-alignment): short wins; the fade cost
   is the pitch DIFFERENCE mixing across it. kSeamFadeMs stays 1.5.

## What is left, honestly

- The current engine on the field ruler (tools/pitch_field_compare,
  sourceNEW/antaresNEW): 51 spans / 0.19s of >0.10 similarity deficit vs
  Antares at vib-off, 0 inversions, worst deficit −0.34. The remaining
  deficit windows sit at span boundaries and marginal creak, where the
  contract's pitch step is genuine.
- Changing this number further means changing the CONTRACT (e.g. not keeping
  consonants bit-exact dry) or the boundary COUNT at the detector — both
  product decisions, not fixes.
- Suspect for the separate vib-on tuning gap: DEFECT_VIBRATO_ON_TUNING_COST.md.
