# The standing reference set (declared 2026-08, reviewer ruling)

Every field investigation starts from THIS material, so numbers stay
comparable across rounds. Do not substitute a new take without a ruling.

## The files

`/Users/SeanD/Music/Logic/test/Bounces/`:

| file | what it is |
|---|---|
| `sourceNEW.wav` | the dry take — 13.25 s, low male, D minor, 48 kHz |
| `antaresNEW.wav` | Antares Auto-Tune Pro on that take, sample-aligned |
| `echojayignoreonNEW.wav` | EchoJay, ignore-vibrato ON — **pre-drift-bleed build** |
| `echojayignoreoffNEW.wav` | EchoJay, ignore-vibrato OFF — **pre-drift-bleed build** |

The two EchoJay bounces pre-date the shipped drift-bleed (commit 98fc274);
they are the "was" columns. Fresh comparisons must re-bounce from the
installed build (verify by UUID, see the install discipline).

The older 8.2 s trio (dry/echojay/antares.wav, same folder) belongs to the
`pitch_ab_test` hard-match gate only.

## The ruler

`tools/pitch_field_compare` — one tool for both sides of any change:
rough spans vs Antares (>0.10 cycle-similarity deficit at the same instant,
merged; count + duration), inversions (must be 0), and tuning (in-scale,
same-semitone vs Antares, improve-rate vs source, median off-grid).
READ ITS CALIBRATION HEADER before quoting absolute tuning numbers:
absolute bars belong to plugin-rendered bounces; offline env-preset renders
are for matched-pair deltas only.

## The bars against this set (as ruled)

- **Rough spans vs Antares**: the binding waveform-continuity number.
  Pre-bleed bounces: 81 spans / 0.35 s (vib off), 98 / 0.48 s (vib on) on
  this ruler (the reviewer's independent ruler read the same story as
  93 / 1.12 s). Current engine, offline matched render: 51 / 0.19 and
  56 / 0.21. The residual is a ruled design property —
  `DESIGN_SEAM_RESIDUAL.md`.
- **Tuning held** means: in-scale ≥95%, same-semitone ≥95%, improve-rate
  ≥58% — judged on plugin-rendered bounces. Antares on this ruler:
  improve 62.4%, off-grid 5.1 c; Sean's vib-off bounce: 61.7%.
- **Inversions**: zero, always.
- The vib-on tuning gap (same-semitone 90.4% vs 97–98% everywhere else) is
  the largest measured open defect: `DEFECT_VIBRATO_ON_TUNING_COST.md`.
