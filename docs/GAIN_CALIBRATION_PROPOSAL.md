# Gain / output calibration — PROPOSAL (roadmap 2.1), prototype measured 5 Oct 2026

Status: PROTOTYPE, Phase B1 of the 4/5 Oct overnight run. Nothing exported, nothing published; the mode is
`ejmap --cert-gain-cal "<product>" [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]` and writes
`<cert dir>/gaincal/<identity>.gaincal.json` (`ej_gaincal_prototype/0`). Pure derivation in `EjmapGainCal.h`, pins G1–G12,
mutants M30–M33 red. Results below are from this Mac, 14 installed compressors, in one pass of ~15 s each.

## Method

A 997 Hz sine at −20 dBFS peak, then at −40, through the plugin with ONE gain-role control at each of 21 evenly spaced
norms, everything else at its instantiate value; the same probe process as the compressor sweep (`--sweep` with the control
as `thr=`, `ref=0`, one level, 1.5 s hold). `measured_db` = output RMS − input RMS. Gain-role controls are the roles'
`output`, `makeup` and `input` (when the input is not the amount control), plus names answering trim / gain / level that the
roles neither roled nor vetoed; word-valued and two-state controls are skipped; the amount control and the threshold
candidates are the sweep's business.

Judgement (at −40 dBFS, the level least likely to sit inside the compression path; −20 is the level-dependence check):
- `relative_db` = measured − measured at the control's own "0.0" label when it has one (Solid Bus Comp's Output sits
  2.35 dB above unity at "0.00"; its labels track relative to that point exactly).
- `display_matches` when |relative − label| ≤ **the label's own resolution / 2**, never less than 0.1 dB: bx_opto's Output
  Gain reads 4.79 at "5 dB" because its norm steps are 1.2 dB and the label rounds — that is not a calibration error.
- `level_dependent` when the two levels disagree by more than 0.5 dB anywhere.
- A unit-less numeric label the output tracks IS a dB label (`display_matches`); one it does not track and is not called dB
  is `not_db_scale` (U2A's Gain: 0–100 %, output 0 → 35 dB) — listed with its curve, never judged against the number.
- `no_effect` (span < 0.1 dB over ≥ 3 positions), `unreadable` (silence: 7X-500's Output with its Input at minimum),
  `words`, `few_numeric_points` (< 3 numeric labels) are listed, not judged.

## Results (this Mac, 5 Oct)

| product | control (role) | verdict | worst off (bar) | level dep. | span |
|---|---|---|---|---|---|
| Solid Bus Comp | Makeup (makeup) | `display_off` | 1.00 (0.10) | 0.05 | 26.2 |
| Solid Bus Comp | Output (output) | `display_matches` | 0.05 (0.10) | 0.05 | 26.0 |
| Unfiltered Audio Zip | Out Gain (output) | `display_matches` | 0.10 (0.50) | 0.00 | 38.1 |
| bx_townhouse Buss Compressor | MakeUp (makeup) | `display_off` | 6.80 (0.10) | 0.01 | 21.8 |
| bx_townhouse Buss Compressor | Virtual Gain (gain) | `no_effect` | 119.43 (0.10) | 0.00 | 0.0 |
| bx_townhouse Buss Compressor | Gain Reduction (gain) | `unreadable` | 0.00 (0.10) | 0.00 | 0.0 |
| bx_townhouse Buss Compressor | Input (input) | `few_numeric_points` | 0.00 (0.10) | 0.00 | 0.0 |
| bx_opto | Output Gain (output) | `display_matches` | 0.40 (0.50) | 0.00 | 24.0 |
| EMO-D5 (s) | Makeup (makeup) | `display_matches` | 0.00 (0.10) | 0.00 | 36.0 |
| C1 comp (s) | Makeup (makeup) | `display_matches` | 0.00 (0.10) | 0.00 | 80.0 |
| C1 comp (s) | Output Gain (output) | `display_matches` | 0.00 (0.10) | 0.00 | 58.0 |
| elysia mpressor | EQ Gain (gain) | `no_effect` | 6.00 (0.10) | 0.01 | 0.0 |
| elysia mpressor | Gain (gain) | `display_matches` | 0.05 (0.10) | 0.51 | 20.0 |
| elysia mpressor | Gain Reduction (gain) | `few_numeric_points` | 0.00 (0.10) | 0.01 | 0.0 |
| SSLComp (s) | Makeup (makeup) | `display_matches` | 0.00 (0.10) | 0.00 | 20.0 |
| RCompressor (s) | Gain (gain) | `display_matches` | 0.00 (0.10) | 10.00 | 60.0 |
| NEOLD U2A | Gain (gain) | `not_db_scale` | 66.23 (0.10) | 0.00 | 42.9 |
| Lindell 7X-500 | Output (output) | `not_db_scale` | 14.30 (0.10) | 0.03 | 38.0 |
| Lindell 254E | Gain (gain) | `display_matches` | 0.00 (0.50) | 3.10 | 20.0 |
| Lindell 254E | Limit Level (gain) | `no_effect` | 0.00 (0.10) | 0.00 | 0.0 |
| Lindell SBC | Gain (gain) | `not_db_scale` | 4.40 (0.10) | 7.15 | 19.6 |
| Lindell SBC | Input Gain (input) | `display_matches` | 0.00 (0.10) | 3.79 | 20.0 |
| Lindell SBC | Output Gain (output) | `display_matches` | 0.00 (0.10) | 0.67 | 20.0 |
| Shadow Hills Mastering Compressor | Optical Gain 1 (gain) | `not_db_scale` | 12.57 (0.50) | 0.01 | 30.7 |
| Shadow Hills Mastering Compressor | Discrete Gain 1 (gain) | `not_db_scale` | 11.44 (0.50) | 0.00 | 30.4 |
| Shadow Hills Mastering Compressor | Optical Gain 2 (gain) | `no_effect` | 23.67 (0.10) | 0.00 | 0.0 |
| Shadow Hills Mastering Compressor | Discrete Gain 2 (gain) | `no_effect` | 23.67 (0.10) | 0.00 | 0.0 |

What this says:
- **Labels are honest where they are dB**: C1 comp Makeup and Output Gain, EMO-D5 Makeup, SSLComp Makeup, RCompressor
  Gain, 254E Gain, SBC Input/Output Gain, mpressor Gain, Zip Out Gain — 0.00–0.10 dB. bx_opto's 0.40 is label rounding.
- **Two make-up labels are off**: Solid Bus Comp Makeup 1.00 dB (reads 1 dB less than labelled at every step above 0),
  bx_townhouse MakeUp 6.80 dB (a labelled dB range the output does not follow — the proposal's first real catch).
- **Input gains cannot be judged where they drive the compressor**: SBC's "Gain" +24 takes −40 dBFS to −16, above its
  threshold, and reads 19.6 dB; level-dependent by 7 dB. An input gain needs a level that stays below the threshold
  after the gain (a third level, −60 dBFS, or the record's own 1 dB point minus the gain range).
- **Several "gain" names are not gains**: EQ Gain (mpressor's sidechain filter), Virtual Gain (townhouse), Gain Reduction
  (both; a meter/limit), Limit Level (words), Shadow Hills' Optical/Discrete Gain 2 (no effect with stage 2 off). The
  roles' vetoes do not cover "EQ" or "Reduction"; the measurement does (no_effect / few points).
- **Shadow Hills Optical/Discrete Gain 1**: unit-less labels the output does not track — stage gains with their own scale.

## Draft export (for Sean; where it lives is his call)

```json
"gain_controls": [
  { "control": "Output Gain", "role": "output", "display_matches": true, "level_dependent": false,
    "gain_curve": [ { "norm": 0.0, "display": "-10.00", "measured_db": -10.01, "relative_db": -10.00 }, "…" ] },
  { "control": "Makeup", "role": "makeup", "display_matches": false, "worst_off_db": 1.0, "gain_curve": [ "…" ] }
]
```
Per control: the measured curve (21 points, both levels), `display_matches` with its bar, `level_dependent`, and the
verdict word. Full ranges with no gaps come for free (every norm has a read-back display).

## Questions for Sean

1. Where does it live — map data (per control, like the range) or a profile field? The server's make-up step (§6.0 rung 4)
   is the consumer.
2. Does the server switch from display to **measured** dB when `display_matches` is false (Solid Bus Comp −1.00,
   townhouse −6.80), or refuse the write?
3. Input gains: judge at a third level (−60) so the reading stays below the threshold, or accept "inside the compression
   path" as the answer for them?
4. CL 1B's Gain (0.33 must read 0.0 dB): needs the iLok — not measurable here; first thing on your Mac
   (`--cert-gain-cal "Tube-Tech CL 1B"`).
