# Saturation — PROPOSAL (roadmap 2.5), prototype started 5 Oct 2026 (partial)

Status: PROTOTYPE, Phase B5 of the 4/5 Oct overnight run, **partial**. Nothing exported, nothing published; the mode is
`ejmap --cert-saturation "<product>" [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]`, writing
`<cert dir>/saturation/<identity>.saturation.json` (`ej_saturation_prototype/0`). It reuses the sweep process unchanged: the
drive-type control (drive / saturation / sat / color / harmonics / warmth / heat / crush / amount, mix excluded) is the swept
control at 11 norms, a 997 Hz sine at −20 then −12 dBFS; `gain` = out − in and the hold line's `tone_frac` (the output's power share
at the tone) gives `harmonics_db = 10 log10 ((1 − tone_frac) / tone_frac)`, a THD+N proxy.

## What the first pass showed (this Mac, 5 Oct)

- **The proxy's floor is −40 dB**: the sweep prints `tone_frac` to four decimals, so anything cleaner than 0.9999 reads as "no
  harmonics". A saturator at moderate drive sits well below that. The honest measurement needs its own numbers: Goertzel bins at
  2f, 3f, 4f, 5f beside the fundamental (the `--response` mode already has exact-bin Goertzels; adding the harmonics of one tone
  is a small change) and the level change per position. Not built tonight.
- **Black Box HG-2 and bx_saturator V2 are inert here** — gain −0.07 dB and tone share 1.0000 at every Saturation / Drive position,
  the same shape as NEOLD V76U73 / U2A / U17 and the `inert` result (A3). All five are Plugin Alliance products on a Mac where other
  PA products (bx_opto, Lindell SBC / 254E / 7X-500, bx_digital V3, AMEK EQ 200, bx_limiter True Peak) process normally: a
  per-product licence state, silent, no window. The saturation pass should run the inert check first and skip inert units.
- bx_yellowdrive and freqtube were not reached (the pass was cut short); kHs Distortion is PACE-bound.

## Proposed method (for the next pass)

A 997 Hz sine at −20 and −12 dBFS (and a third level, −6, for units that saturate on level), the drive control at 11 norms; per
position: gain change (dB), THD (2nd–5th harmonics against the fundamental, by exact Goertzel bins), the even/odd balance
(2nd + 4th against 3rd + 5th — the "warm vs hard" character), and level dependence (the three levels). Export a `saturation`
block: `drive_map: [{norm, display, gain_db, thd_db, even_odd_db}]` with the level it was measured at.

## Questions for Sean

1. Is THD at 997 Hz the figure the server wants for "more saturation", or a feel ladder like the tuner's (clean / warm / driven /
   crushed) set by ear?
2. Level dependence: saturation is by nature level-dependent; which level does the server assume when it writes a drive?
