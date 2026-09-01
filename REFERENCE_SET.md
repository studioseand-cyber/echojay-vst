# The standing reference set (declared 2026-08, reviewer ruling)

Every field investigation starts from THIS material, so numbers stay
comparable across rounds. Do not substitute a new take without a ruling.

## The files

`/Users/SeanD/Music/Logic/test/Bounces/`:

| file | what it is |
|---|---|
| `sourceNEW.wav` | the dry take — 13.25 s, low male, D minor, 48 kHz |
| `antaresNEW.wav` | Antares Auto-Tune Pro on that take, sample-aligned |
| `echojayignoreonNEW.wav` | EchoJay **at HARD**, ignore-vibrato ON |
| `echojayignoreoffNEW.wav` | EchoJay **at HARD**, ignore-vibrato OFF |

SETTINGS AUDIT (2026-08-29): the two EchoJay bounces are HARD-preset
bounces - their tuning AND rough-span columns match a current-engine hard
render (84/0.34s, 63.5%, 4.1c vs their 81/0.35s, 61.7%, 4.4c), not a
natural one (51/0.19s, 49.4%). An earlier revision of this file called
them pre-drift-bleed "was" columns and credited the bleed with a ~40%
field improvement; that comparison was CONFOUNDED - it compared natural
renders against hard bounces. At hard, the bleed's field effect is ~nil
(the 3c cap is a sliver of hard's sustained shifts). Always compare at
matched settings.

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

- **Rough spans vs Antares**: the binding waveform-continuity number,
  BY OPERATING POINT (current engine, this ruler, same take):
    natural: 51 / 0.19 s (vib off), 56 / 0.21 s (vib on), 0 inversions
    hard:    84 / 0.34 s (vib off), 94 / 0.44 s (vib on), 3 inversions
      at vib-on, worst deficit -0.95
  Sean's complaint lives at HARD (his bounces measure 81/0.35 and
  98/0.48). The natural-point residual is a ruled design property
  (`DESIGN_SEAM_RESIDUAL.md`); the hard point is under investigation.
- **Tuning held** means: in-scale ≥95%, same-semitone ≥95%, improve-rate
  ≥58% — judged on plugin-rendered bounces. Antares on this ruler:
  improve 62.4%, off-grid 5.1 c; Sean's vib-off bounce: 61.7%.
- **Inversions**: zero, always.
- The vib-on tuning gap (same-semitone 90.4% vs 97–98% everywhere else) is
  the largest measured open defect: `DEFECT_VIBRATO_ON_TUNING_COST.md`.

## Standing instruction (1 Sep 2026 ruling)

**Ear renders are made at Sean's ACTUAL session settings, read from his
session, never assumed** — including voice_type. The alto_tenor-default
round was lost because every offline instrument pinned low_male while his
session sat on the default; the defect only existed at his settings.
Every measurement records its voice_type (see PITCH_P0_VALIDATION.md
§17.5 item 5).
