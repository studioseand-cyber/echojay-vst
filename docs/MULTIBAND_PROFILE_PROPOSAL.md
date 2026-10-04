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
