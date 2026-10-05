# Saturation — PROPOSAL (roadmap 2.5), prototype 5 Oct 2026 (harmonic measurement built, overnight run 2 R3)

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

## The harmonic measurement (5 Oct, R3 — built)

The proxy is gone. `--cert-saturation` now runs the probe's `--response tones=1 harmonics=5` — one 997 Hz sine snapped to
an exact Goertzel bin, the drive control at 11 norms, one process per level (−20, −12, −6 dBFS peak) — and reads, per
position, the fundamental's gain (out − in) and the 2nd–5th harmonics at their own exact bins, relative to the fundamental.
`EjmapSaturation.h` derives: **THD** (the harmonic power sum against the fundamental, dB and %), **even/odd** (2nd+4th against
3rd+5th, dB, only when both sides are above −100 dB; otherwise the character word `even` / `odd`), the **1 % and 0.1 %
onsets** (the first position by norm whose THD reaches −40 / −60 dB, with its display), the **gain span** and **THD span** over
the positions, and three verdict shapes a position curve can take instead of a drive law: **inert** (fundamental within 0.05 dB
and no harmonic above −90 dB everywhere — the processing never ran), **silent** (the input carried the tone, the output holds
nothing below −150 dB — no output at all), **no effect** (the product distorts from its other settings, this control changed
neither level nor THD across its positions). Record: `<cert>/saturation/<identity>.saturation.json`, `ej_saturation_prototype/1`.
Pins S1–S15 (RoundTripTest), mutants: THD summed in amplitude, the 1 % onset on the 0.1 % bar, inert ignoring harmonics — red.

### Measured on this Mac (5 Oct; `cert-traces/2026-10-05-saturation/`)

| product | control | at −12 dBFS, drive from bottom to top | character | 1 % onset (−20 / −12 / −6) | THD at top (−20 / −12 / −6) |
|---|---|---|---|---|---|
| J37 (s) | Saturation 0→30 | THD −63.7 → −10.6 dB, gain −0.45 | odd (h3 −11, h2 −90) | 18.7 / 9.4 / 4.0 | −14.5 / −10.6 / −9.8 |
| NEOLD BIG AL | Drive 0→56 | THD −52.7 → −10.6 dB, gain −8.1 | even at low drive (+24 dB) → odd at the top (−13) | 50.4 / 50.4 / 22.4 | −17.3 / −10.6 / −8.8 |
| elysia karacter master | Drive 1 / Drive 2 0→11 | THD −41.1 → −9.4 dB (33 %), gain −14.2 | odd (h3 −10.6) | 4.4 / 2.2 / 0.0 | −9.5 / −9.4 / −9.3 |
| elysia karacter master | Color 1 / 2 | THD −41.3 → −40.9 (no THD change), h2 moves | mixed | never (−6: at the bottom) | −53.8 / −40.9 / −30.0 |
| Looptrotter SA2RATE2 | Drive 1 / Drive 2 0→100 % | THD −43.4 → −15.1 dB, gain +4.7 | even (+18.6) → odd (−14.6) | 30 % / 10 % / 0 % | −19.3 / −15.1 / −16.4 |
| MSaturator | Even harmonics 0→500 % | THD −43.7 → −18.6 dB, gain +3.7 (constant) | even (+25 at top) | 100 % / 50 % / 0 % | −25.8 / −18.6 / −16.0 |
| MSaturator | Harmonics − Gain / 2nd / 3rd / 4th / 5th | THD −37.5 at every position | — | — | **no effect** (the product distorts from its default state; these controls did not move the tone) |
| CamelCrusher | CompressAmount 0→1 | THD −187 → −14.5 dB, gain +6.1 | odd only | 0.5 / 0.3 / 0 | −12.9 / −14.5 / −11.6 |
| Saphira (s) | Warmth Return 0→… | THD −31 → −21.7 dB at −12 (the drive of the Warmth section); Warmth Send, Band gains/freqs: THD within 2 dB (not drives; the name rule took them) | — | at the bottom (the default state distorts) | — |
| NLS Buss (s) | VCA 1–5 Drive | THD −43.7 at every position | — | never | **no effect** shape on every VCA drive (only the selected VCA processes; the selector was left as instantiated) |
| bx_yellowdrive, freqtube | Drive | **silent**: output below −150 dB at every landed position while the input carried the tone | — | — | the third Plugin Alliance licence shape (inert = passes untouched; silent = passes nothing) |
| Kramer Tape, Aphex Vintage Exciter, Kiive Tape Face, Spectre, ADPTR Hype | — | 0 drive-type controls by the name rule (Record Level / Flux, Harmonics?, Tape, Drive?) | | | |

What the numbers say:
- **Onset is level-dependent in every unit that saturates** (J37's 1 % point moves from '18.7' at −20 to '4.0' at −6; SA2RATE2 from
  30 % to 0 %), so a profile has to carry the level it was measured at — the second question for Sean below is the first.
- **The gain law and the THD law are separate things**: karacter's Drive loses 14 dB of level while reaching 33 % THD; MSaturator's
  gain is constant across its drive; CamelCrusher gains 6 dB. `gain_db` per position is in the record beside `thd_db`.
- **Character changes along the drive** (BIG AL and SA2RATE2 go from even-dominant to odd-dominant); one even/odd figure per unit
  would be wrong — it is per position in the record.
- **The name rule for "the drive control" is the weak part**: it took Saphira's band gains and MSaturator's per-harmonic trims, and
  missed five units with no drive word. The honest rule is the measurement (a control is a drive when its THD span across positions
  is above a bar); the `no_effect` verdict already carries that, so a second pass could keep every numeric control and let the THD
  span decide — a change for after a ruling, not tonight.
- **Timeouts**: Saturn 2 refused `--text-at all` in 120 s (FabFilter's text reads are slow on a 60-control unit) — not measured.

### Questions for Sean (unchanged, plus)

3. Which drive controls count: by name (today), or any control whose THD span across its positions exceeds a bar (the measurement)?
4. Onset at which level: the profile could carry the 1 % onset per level (−20 / −12 / −6) and let the server pick by the track's level.
