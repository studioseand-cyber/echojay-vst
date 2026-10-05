# De-essers — PROPOSAL (roadmap 2.9), prototype 5 Oct 2026 (overnight run 2, R6)

Status: PROTOTYPE. Nothing exported, nothing published. Mode: `ejmap --cert-deesser "<product>" [--out <cert dir>]
[--probe <path>] [--ejmap-ledger <dir>]`, writing `<cert dir>/deesser/<identity>.deesser.json` (`ej_deesser_prototype/0`).

## The measurement

The compressor engine with the tone in the sibilance band (`EjmapDeesser.h`, pure):

- **The ladder.** The probe's `--sweep` with a **6.5 kHz** sine (and **997 Hz** as the control — a de-esser should not react
  below its band), the threshold control at 6 norms, levels −30 / −24 / −18 / −12 / −6 dBFS, one process each. GR per
  position and level = the gain at the open end (the position that reduces least at the loudest level) minus the gain at the
  position. Threshold → GR without a reference process; the two tones give the band selectivity in one line.
- **The centre and the shape.** The probe's `--response` (121-tone multitone, −12 dBFS) at the hardest threshold (most GR at
  −12 on the ladder) against the open end: the deviation per tone. Its deepest point is refined parabolically; the shape is a
  **notch** when the cut recovers above the deepest point (a band-pass detector / split band) — the figure is the centre — or a
  **shelf** when the cut holds to the top of the grid (a high-pass detector) — the figure is the half-depth corner. The frequency
  control's display is compared with the shape's own figure, at three positions.
- **The mode.** On the same deviation: split band when 997 Hz is within 1 dB while the band is cut 3 dB or more; wideband when
  997 Hz is cut within 1.5 dB of the band; else partial. Read per text of a mode switch where one exists.
- Band-limited noise (4–10 kHz) was not built: the 121-tone multitone already carries that band and read the shapes; a noise
  generator is a probe change for after the ruling.

Pins L1–L3, C1–C4, H1 (RoundTripTest).

## Measured on this Mac (5 Oct; `cert-traces/2026-10-05-deesser/`)

| product | threshold control | ladder: max GR at 6.5 kHz / at 997 Hz | centre / corner vs the Freq label | mode |
|---|---|---|---|---|
| DeEsser (s) (Waves) | Threshold | **11.7 dB / 0.00 dB** | shelf (high-pass): corners 2709 / 6938 / 13939 Hz for labels 2000 / 5657 / 16000 (+35 / +23 / −13 %) | Audio: Split / Wideband — at 2000 the band leaks to 997 (−5.4 dB: partial), at 16000 split |
| RDeEsser (m) | Range | **18.5 / 0.00** | shelf: corners 1453 / 3955 for 2000 / 5657 (−27 / −30 %); at 16000 nothing cut 3 dB | FilterType BandPass / HighPass (the default read as a shelf) |
| Sibilance (s) | Threshold | 7.7 / 0.00 | no frequency control; nothing cut 3 dB on the multitone (the detector needs more than the multitone's share at the band) | — |
| Lindell 902 De-esser | Range | **19.0 / 0.00** | wideband at the default (Mode HF Only = Off): the whole spectrum cut −13 dB, the Frequency label unreadable in that mode; HF Only On: shelf, corner 7065 Hz, 997 Hz untouched | **Off = wideband, On = split band** — the switch does what it says |
| SPL De-Esser | S-Reduction | **17.6 / 1.5** | notch at 6362 Hz (no frequency control: a fixed band) | split band in Left/Right and Mid/Side; the "Side" position would not land |
| MannyM TripleD (s) | DeBoxy Thresh (the name rule took the DeBoxy section's threshold, not the DeEss one) | 0.0 / 0.5 | — | — |
| TB_Sibalance_v4 | Range | 5.1 / 0.00 | "Stop freq" is not the centre (10000..19000 Hz labels); nothing cut 3 dB on the multitone | — |
| smartDeess | Max. Gain Reduction | no readings (the unit needs its learn / sidechain state) | — | Processing Mode Full Range / Split Band (not reached) |
| AVOX SYBIL | `--list-params` exit 3 (refused) | | | |

What the numbers say:
- **Band selectivity is clean on every de-esser that works**: 5–19 dB of GR at 6.5 kHz against 0.0 dB at 997 Hz (SPL 1.5 dB) —
  the ladder on the sibilance tone is the right engine, and the 997 Hz control row is the one-line proof the unit is a de-esser.
- **"Freq" is a corner, not a centre, on the Waves units** (DeEsser, RDeEsser): the cut holds to 20 kHz; the half-depth corner
  sits 13–35 % from the label — the label is the detector's filter frequency, the cut's corner is where the audio loses half the
  depth. Which of the two the server should write is the question below.
- **Lindell 902's mode switch is literal**: wideband cuts everything −13 dB at the default; HF Only turns it into a split band
  with a 7 kHz corner. A profile has to carry the mode.
- The multitone drives some units too softly at the band (Sibilance, TB_Sibalance: 5–8 dB on the ladder, under 3 dB on the
  multitone): the centre run should use the ladder's level and a signal weighted to the band (the noise the roadmap asked for).
- Name-rule misses: TripleD's three sections, TB's "Stop freq", smartDeess's learn state, SYBIL's refusal.

## Questions for Sean

1. The frequency figure: the label (detector), the corner (where the cut is half), or the centre (notch units)?
2. Mode in the profile: carry split / wideband as measured, and the switch control where one exists?
3. Threshold → GR at which level: the ladder gives five; the server's vocal sits around −18 dBFS RMS.
