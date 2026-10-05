# Multiband profiling — the survey of Sean's 18 and a proposal for Kathy to take to Sean (4 Oct 2026; read-only, no code)

The full per-product listing (every global control with range and default, every crossover, every per-band threshold, and
which candidate the 997 Hz tone certified) is `tools/ejmap/cert-traces/2026-10-04-sean-zip/multiband-survey.txt`.

## What the 18 look like

| product | bands | global amount control | default crossovers | per-band thresholds (default) | 997 Hz landed in |
|---|---|---|---|---|---|
| Waves C4 (m)/(s) | 4 | none ("Behavior" is a mode) | 92 / 4000 / 11071 Hz | Band 1–4 Thresh −27.0 / −19.2 / −30.1 / −35.3 | Band 2 |
| Waves C6 (m)/(s), C6-SideChain (m)/(s) | 6 (4 + 2 floating) | none | 92 / 4000 / 11071; floating bands at 1000 and 5003 Hz | Band 1–6 Threshold 0.0 (all) | Band 3 |
| Waves LinMB (m)/(s) | 5 | none (Trim, Adaptivity are not amount) | 92 / 545 / 4000 / 11071 | Band 1–5 Thresh 0.0 | none (Band 3 nonmonotonic, 4 flat) |
| Drawmer 1973 | 3 (+ 3 "S" sidechain thresholds) | Mix (WET / 5.0 / DRY — a word-ended range) | none exposed | Low/Mid/High Threshold 0.0 | none — all 6 flat |
| Lindell 354E | 3 | Mix 0–100 % (100 %) | 200 Hz / 5000 Hz | Low/Mid/High Threshold −4.0 dB | **Low AND Mid** (997 Hz sits near the 200 Hz band edge? — two bands respond) |
| Lindell MBC | 3 | Mix 0–100 (100) | none exposed | Low/Mid/High −1.61 | Mid |
| SSL G3 MultiBusComp | 3 | Mix 0–100 (100) | Low 120 / High 6000 Hz (+ bell EQ freqs) | Low/Mid/High 0.0 | Mid |
| MO-TT (OTT clone) | 3 × (downward + upward) | **Amount 0–100 %** (0 %) — a true global depth | Low/Mid/High freqs read "Off" | six thresholds at −80 dB | none (unreadable/nonmonotonic: the Amount at 0 % disables everything) |
| FabFilter Pro-MB | 6 | Mix 0–200 % (100 %) | per band Low/High Crossover, all 30 Hz–30 kHz (bands not configured) | Band 1–6 Threshold −18 dB | none — all flat (no band is active at instantiate) |
| iZotope Ozone 12 Dynamics | 4 × (comp + lim), Main and Aux | Global Gain ±30 (0); no depth | not exposed as parameters | Band N Comp −12 / Lim 0 | Band 1 Comp AND Band 1 Lim |
| Melda MDynamicsMB / MBLarge | 4 (+ gate + 2 processors per band) | **Compression (Globals) −100…+100 %** (0 %), Dry/wet, Tone | Cross 1/2/3 = 200 / 1000 / 4634 Hz | Threshold (Band N) −20 dB | nothing measured: licence_suspect (silent) |
| Melda MTurboCompMB | 6 | Dry/Wet 0–100 % | per-band detector freqs 600 Hz | Band N Input (Globals) 0 dB (input-as-threshold) | nothing measured: licence_suspect |

Three families: **(i) global-depth units** (MO-TT Amount, Melda Compression (Globals)) where one control IS the amount;
**(ii) per-band-threshold units with a Mix** (Lindell, SSL, Drawmer, Pro-MB) where Mix is a wet/dry, not a depth; **(iii) pure
per-band units with nothing global** (Waves C4/C6/LinMB, Ozone). Only one band ever certifies on the 997 Hz tone (two on the
354E and Ozone because the tone sits near a crossover / the limiter stage), so a profile from today's sweep describes one
band's threshold against a sine — not what a vocal through the whole unit would get.

## Proposal (for Kathy and Sean to agree before any code)

1. **Test signal.** Keep the sine where it works: one 997 Hz-style tone per band, at each band's geometric centre between the
   default crossovers (C4: 30–92 → 53 Hz, 92–4000 → 607 Hz, 4000–11071 → 6.7 kHz, 11071–20k → 14.9 kHz), swept as today, so
   every band gets a 1–12 dB curve of its own. Add ONE vocal-spectrum signal (pink noise shaped to the spec's vocal, −18.4
   dBFS RMS, the same window and hold) for the whole-unit reading; the sine per band gives the thresholds, the noise gives
   what the vocal actually gets. (Pure vocal-noise alone cannot give per-band thresholds: the bands interact.)
2. **How GR is read.** Per band: as today, the band's own sine, GR = its quiet-reference gain minus the gain at level — the
   reference ladder per band exactly as for a single-band unit. Whole unit: one figure, the vocal-weighted level drop of the
   shaped noise, read on the same A-weighted-ish vocal curve the shaping used, so a band outside the vocal's energy cannot
   mask the number. Both go in the profile; the server picks by the whole-unit figure and reports the band figures.
3. **The amount, two kinds.** (a) A **global control** where one exists (MO-TT Amount, Melda Compression): the sweep walks
   it as the amount, every band threshold at instantiate, exactly the single-band profile with `topology: "multiband_global"`.
   (b) **No global control**: the amount is a **common dB offset applied to every band threshold** — the sweep writes
   `threshold_i = default_i + offset` for offset −30…0 dB in the usual steps (one process per offset), `topology:
   "multiband_offset"`, `amount.control` naming all band thresholds and `amount.curve[].norm` replaced by `offset_db`; the
   server writes the same offset to each band. This keeps the user's band balance (their defaults) and moves the whole unit
   harder or softer, which is what "more compression" means on a multiband. Mix stays in neutral at its instantiate value.
4. **What is NOT proposed:** picking one band's threshold as "the" amount (today's accidental result), or a per-band ask from
   the server (no vocal brief says "more on the lows").
5. **Scope and cost.** Nothing changes for single-band units. For the 15 multibands here the sweep grows by the band count
   (one sine sweep per band + one noise sweep): 4–7 × ~2 min per product, roughly 2–3 hours of batch time on Sean's Mac for
   the set, resumable as today. Melda's three need their licence first.

Decision needed from Sean: the schema fields (`topology`, `offset_db`, per-band `in_at_gr` arrays, the whole-unit figure's
name) and whether the server will write a common offset to N controls. Until then the 18 stay `multiband: profiling not
built yet`.

## The prototype with real numbers (5 Oct 2026, overnight run 2, R7 — no spec change)

Built as `ejmap --cert-multiband "<product>"` (`EjmapMultiband.h`, record `<cert>/multiband/<identity>.multiband.json`,
`ej_multiband_prototype/0`; `cert-traces/2026-10-05-multiband/`). Item 1's signals and item 3's two amounts exactly as proposed:

- **Bands** from the crossover controls' instantiate displays (20 .. x1 .. xn .. 20000 Hz, geometric centres; "Off" and
  non-numeric displays skipped and named). **Per band**: the probe's `--sweep` at the band's centre tone, that band's
  threshold at 6 norms over levels −30..−6 dBFS, GR against the open end (the least-reducing position).
- **The whole unit** on a vocal-shaped signal: the probe's `--response` gained `shape=vocal` — the 121-tone multitone weighted
  pink below 1 kHz, −12 dB/oct below 100 Hz, −6 dB/oct more above 1 kHz (a speech-like long-term spectrum) — at five levels;
  gain = total output power over total input power across the tones; GR against the unit as instantiated.
- **The amount**: the global control at 6 norms where one exists (`topology: multiband_global`), else a common dB offset
  0 / −6 / −12 / −18 / −24 written to every band threshold from each control's own dB ends (`multiband_offset`).

| product | bands (centres) | per-band max GR on its tone | whole-unit GR on the vocal signal (−18 / −12 / −6 dBFS) | amount family |
|---|---|---|---|---|
| Waves C4 (s) | 4: 43 / 607 / 6655 / 14880 Hz from 92 / 4000 / 11071 | 6.5 / 7.8 / 5.2 / 6.2 dB | offset −6: 0.9 / 1.7 / 1.6; −12: 2.6 / 3.3 / 2.8; −18: 4.2 / 4.5 / 3.8; **−24: 5.4 / 5.4 / 4.3** | offset on Band 1–4 Thresh (−27 / −19.2 / −30.1 / −35.3 → −51 / −43.2 / −54.1 / −59.3) |
| Waves LinMB (s) | 5: 43 / 224 / 1476 / 6655 / 14880 | 4.9 / 5.2 / 5.3 / 4.3 / 4.9 dB | −24: 0.5 / 1.7 / 3.0 (thresholds at 0.0 dB: the offset only bites the loud levels) | offset on Band 1–5 Thresh |
| Waves C6 (s) | 6 by the edge list — **but the two floating bands (1000 / 5003 Hz) are not edges**: the pairing put Band 5 / 6 on the wrong tones | 0.05 / 0.09 / 6.4 / 3.5 / 1.1 / 0.0 (bands 1, 2, 6 read on tones that are not theirs) | −24: 1.3 / 2.3 / 3.4 | offset on Band 1–6 Threshold; the crossover rule needs a floating-band case |
| Lindell 354E | 3: 63 / 1000 / 10000 from 200 / 5000 | 8.4 / 15.6 / 10.4 dB | −6: 0.3 / 2.1 / 2.0; −12: 2.5 / 4.1 / 5.1; **−18 and −24 identical (3.8 / 6.0 / 7.7)**: the thresholds end at −20 dB, the offset clamps there (the displays say so) | offset on Low / Mid / High Threshold (−4.0 → −20.0) |
| Xfer OTT | no crossovers exposed (0 bands; Thresh L / M / H unpaired) | — | as instantiated (Depth 100) the unit GAINS +11.9 / +10.6 / +5.9 dB at −30 / −24 / −18 and loses 5.6 at −6 (upward + downward); Depth 0 removes all of it | **global** Depth 0..100: the "GR against instantiate" reads +11.9 at Depth 0 because the instantiate state is the full effect — for a global-depth unit the open reference must be the control's zero |
| Melda MDynamicsMB | 6 from Cross 1/2/3 + two detector freqs taken as crossovers (20 / 24 / 50 / 200 / 1000 / 4634): **the name rule took 22 "thresholds"** (gate and processor ones per band) and paired band 5 / 6 with Band 1's gate / processor | 38.8 / 17.8 / 6.8 / 27.3 / 30.6 / 30.0 on the paired controls (not all the band's own) | Globals → Compression −100..+100 %: −100 / −60 / −20 → 0.00; +20 → **−2.4** (gain), +60 → −7.2 / −6.4 / −0.8, +100 → −11.9 / −6.5 / −1.6 at −30 / −18 / −6 | **global**; the positive side makes GAIN at quiet levels (upward compression), so the sign of "GR" needs saying per level |
| MO-TT | PACE activation window at `--list-params`: not loaded (the licence rule) | | | |

What the numbers say about the proposal:
1. **The offset family works where the thresholds are plain dB controls** (C4, LinMB, 354E): monotonic in the offset, the band
   balance kept, and the whole-unit figure on the vocal signal is what a vocal gets — C4 at −24 reads 5.4 dB while its single
   bands read 5–8 dB on their own tones. The control's END must be respected (354E clamps at −20: two offsets collapse into
   one — the record's displays show it; the profile should stop at the last distinct offset).
2. **The global family needs its own open reference**: OTT's and Melda's instantiate state is not "off"; the reference for a
   global-depth unit is the depth control at its zero, and the GR sign has to be carried per level (OTT and Melda both make
   gain at quiet levels — upward compression — which "GR" as a positive number cannot express).
3. **The crossover rule needs two cases the survey did not have**: floating bands (C6's bands 5 / 6 sit INSIDE other bands, not
   between edges) and Melda's per-band detector frequencies (not crossovers). Pairing bands with thresholds by order is wrong
   for both; the pairing should use the threshold's own band word / number against the crossover's.
4. **The name rule for "threshold" over-collects** (Melda's gate and processor thresholds): the proposal's "every band
   threshold" must mean the compressor's, which on Melda is "Band N → Threshold" — the arrow-less names are other stages.
5. Cost as measured: 35–43 processes per unit, 3–6 minutes each here.

Decision needed from Sean is unchanged (schema fields, the common-offset write); the numbers above are what those fields
would carry for the three families.
