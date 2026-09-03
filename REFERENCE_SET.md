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

## The matched-settings block (1 Sep 2026 — VERIFIED FROM BOTH UIs, not assumed)

The first controlled comparison of the investigation. τ400 renders
(antaresnew1 / echojaynew1) were made at:

    EchoJay:  voice ALTO/TENOR (amber mismatch warning shown), formant
              preserve, retune 400ms, flex 0, humanize 0, natural_vibrato
              0 (custom), key D minor by hand, reference 440.0 (auto,
              self-not-followed), mixing latency 38ms full lookahead.
              Octave guard: 71 fires on the take.
    Antares:  Input Type Alto-Tenor, Key D minor, Retune 400, Flex 0,
              Humanize 0, Natural Vibrato 0.0, Tracking 50, Detune 440.0,
              Transpose 0, Formant on, Mix 100.

Same voice type, same retune, same key/scale/reference, all shaping at
zero. The 5.2s event (EchoJay 161.5Hz off-grid vs Antares 173.5 faithful)
has NO settings difference left to explain it.

**Standing instruction, second half (added after this round): read the
REFERENCE plugin's settings too.** The rule's first half was written
after assuming EchoJay's voice type; the identical assumption was then
made about Antares for a month.

## antares_retune0_NEW.wav (Desktop, 2 Sep 2026 16:31) — settings confirmation

Retune Speed 0, confirmed by Sean BY HAND on 2 Sep 2026 — no screenshot
exists for this bounce. Audio evidence concordant: chromatic-tone occupancy
0.6% (= the source's own rate, so no chromatic targeting; key D minor
consistent), and the onset off-grid collapse (med 1.94c) is only reachable
at a very fast retune. Comparison against EchoJay tau6 is FAIR per the
§17.3 floor calibration (Antares 0 ≈ our 4–6 ms). All other settings per
the 1 Sep matched block.

## PROVENANCE AMENDMENT (3 Sep 2026, round-18 ruling C; DEFECT_AUTOKEY_PROVENANCE.md)

The two standing EchoJay bounces (echojayignoreonNEW / echojayignoreoffNEW,
29 Aug 16:42) were made 46 minutes BEFORE the circular-reference guard
(1c5fb52, 17:28) and are tuned to 438.99 / 439.14 Hz - 3.4 / 4.0c flat of
the 440 grid their off-grid columns were measured against, 2.1 / 2.7c flat
of the source's own centre (439.68). Every absolute tuning number quoting
them - including this file's "Sean's vib-off bounce: 61.7%" - carries that
bias. Their applied KEY is recoverable only to an equivalence class
(D minor / F major / C major / A minor / chromatic: identical on this
phrase's E-F-G sustained content; every damaging wrong key is excluded).
In-process renders (440 by construction, key fixed) remain the matched-
pair instrument; the old 8.2 s trio's members were made under DIFFERENT
keys and references (dry.wav is not in D minor; echojay3 at 434.5 Hz) and
are for the hard-match gate ONLY, never for tuning. Until a bounce
protocol records the [DETECTED KEY] readout at bounce time, every new
bounce carries this risk.
