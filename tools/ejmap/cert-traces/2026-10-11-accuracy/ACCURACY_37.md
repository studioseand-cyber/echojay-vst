# Accuracy passes vs the 37 published profiles (11 Oct)

These are the accuracy passes A1–A4 on the merged copy (cert 2 + cert 3 + Sean's 11 Oct zip): combined, material, frequency and samplerate. Nothing was loaded and nothing was sent.

All 37 profiles have frequency, material and combined records. Only C1 comp (m), mpressor, Solid Bus Comp and SSLComp (s) also have a samplerate record (the pass samples ten products). No samplerate flag fired.

| file | what it holds |
|---|---|
| `flags_37.py` / `flags_37.txt` | every flag, per product |
| `quiet_gain_at_pick.txt` | measured vs predicted quiet gain for every combined run |
| `timing_positions_over_0p5.txt` | the time-draft positions that move GR by more than 0.5 dB |

## 1. The 37: what each flag means and whether to hold

Thresholds (Kathy):
- frequency: `frequency_dependent`, which means GR moves more than 1 dB across 100 / 997 / 5000 Hz.
- material: the median miss on vocal, drums or mix is over 1 dB.
- combined: the GR miss is over 0.5 dB.
- make-up: the output miss net of GR is over 0.5 dB.

| product | flags | hold for v1? |
|---|---|---|
| elysia mpressor | **frequency** −1.52 dB at 100 Hz; combined GR +2.14 | **HOLD** until `gr_by_hz` ships |
| H-Comp (m) / (s) | **frequency** −1.79 dB at 100 Hz; output −3.83 (the quiet gain at the pick is 2.4 dB under `static_gain_db`, no `level_coupling`) | **HOLD** for the same reason, and the output is unexplained |
| VComp (m) / (s) | output −16.6; material drums/mix | **HOLD unless the server uses `level_coupling`** (§2.4). The GR side is fine |
| U73b Compressor | output −10.85; material on all three kinds | same as VComp |
| SSL LMC+ | output −6.70 | same as VComp |
| Empirical Labs Mike-E Comp | output +1.44 (Drive coupling); combined GR +0.81; material mix +1.13 | same as VComp (fixed by `level_coupling`: −0.09) |
| C1 comp (m)/(s), C1 comp-sc (m)/(s) | combined GR +1.26 (attack 0.01 + release 1); material mix +1.03 | no: timing refusal (§2.3) |
| DPR-402 (m) | combined GR +1.17; material mix +1.14 | no: timing refusal |
| Shadow Hills Class A / Mastering | combined GR −1.51 / −3.56 | no: timing refusal (Sean's ruling) |
| Solid Bus Comp | combined GR −2.96 | no: timing refusal (Sean's ruling) |
| SSLComp (m)/(s) | combined GR −0.59; material on all three kinds | no: timing refusal; material flag only |
| API-2500 (m)/(s), Bettermaker, bx_townhouse, C1 comp-gate (m)/(s), kHs, Lindell 254E, Pro Audio DSP DSM V3, UnFairchild | material only | no: material is a model limit (§2.2) |
| dbx-160 (m), Lindell SBC, MCompressor, SSL Native Bus Compressor 2, Unfiltered Audio Zip, RCompressor (m)/(s), TBTECH Cenozoix, Tube-Tech CL 1B | none | no |

"No output measured" (API-2500, mpressor, kHs, Lindell 254E, RCompressor, Shadow Hills ×2, TBTECH, CL 1B) is not a flag. The gain draft has no writable make-up or output control, so the pass had nothing to predict.

So the hold proposal is 3 for frequency (mpressor, H-Comp ×2). There are 5 more output-level holds (VComp ×2, U73b, LMC+, Mike-E), and those lift as soon as the server reads `level_coupling` at the pick.

## 2. Proposals (not built)

### 2.1 frequency_dependent → `gr_by_hz` in the profile

**The data.** Three of the 37 are flagged. All three lose GR at 100 Hz and none at 5 kHz, which looks like a sidechain high-pass in the default state. That is untested.

**What the profile carries:**
```
"frequency": { "gr_by_hz": {"100": 0.49, "997": 2.01, "5000": 2.02}, "g_db": 2.0, "pick_norm": ..., "frequency_dependent": true }
```
This is copied from the A3 record at the g 2 pick.

**How the server corrects.** It splits the track into three bands, using the same band split as the meters' macroBands (low < 300 Hz, mid, high > 3 kHz). Then:
- It takes each band's energy share w_i from the track's spectrum, measured with the same pink reference as the meters.
- Expected GR ratio: r = Σ w_i · gr_by_hz[band_i] / gr_by_hz[997].
- It dials for g / r instead of g, and refuses if r < 0.5, because the unit hardly reacts to that track.

**The stated assumption:** the ratio measured at g 2 holds at other g. The pass only measured at g 2. A later pass at g 4 / g 6 would test it.

### 2.2 material → a per-kind offset AND a `program_dependent` flag

**What the miss actually measures.** The material prediction is the static curve applied to each 50 ms window's RMS, with no time constants (`EjmapMaterial.h` `predictGr`). Splitting each flagged material's windows shows two separate things:
- **Windows the curve predicts as 0 GR,** because they are below threshold. There the unit still reads 1.7–3.4 dB on drums and mix: 68 to 108 of about 100–120 windows. That is GR held between hits by the release, so the static model cannot carry it. For API-2500 and Lindell 254E this is nearly the whole flag. Above the curve their miss is +0.05..+0.5 dB (254E drums −0.31, mix +0.89).
- **Windows above the curve, still missing +1.3..+2.5 dB:** SSLComp, DSM V3, U73b and Bettermaker. That is more GR than the sine curve gives at equal RMS. It fits peak-sensitive detection on high-crest material, but the mechanism is NOT tested.

**What the profile carries:**
```
"material": { "g_db": 2.0, "by_kind": { "vocal": {"median_miss_db": .., "p90_abs_miss_db": .., "median_miss_above_curve_db": ..},
                                        "drums": {...}, "mix": {...} },
              "program_dependent": true }
```
`program_dependent` is true when any kind's median miss is over 1 dB.

**What the server does.**
- If it knows the track's kind, it dials for g − median_miss_above_curve (clamped at 0), using only the above-curve part. The below-curve part is release memory: it depends on the track's dynamics, not on its level, so a fixed offset would be wrong elsewhere.
- If `program_dependent` is true, the card says "this unit reacts to the material: expect more than asked on drums / mixes".
- It is valid near g 2 only, because that is where the pass ran.

**Test first.** Before any offset ships, re-derive the material prediction with the profile's `detector_f` (crest-aware input) and see whether the above-curve miss closes. That costs no new run, but the per-window peak would have to be in the trace. Today the trace carries RMS only, so this needs a probe change.

### 2.3 combined misses → v1 refuses the timing positions that move GR more than 0.5 dB

The time draft measures each position's GR step against the instantiate position at the pick. The combined pass shows these steps do not add up: predicted 9.65 against measured 6.69 for Solid Bus Comp. So v1 writes no position whose |gr_shift| is over 0.5 dB. The units in Sean's ruling:

| unit | positions v1 may write (within 0.5 dB) | refused (more than 0.5 dB) |
|---|---|---|
| SSL Native X-Comp v6 | attack 3.90, 10.64; release 52.03–360.65 | attack 0.50, 1.02, 22.51–100; release 1.00–37.86 and 435.79+ (up to −6.42 dB) |
| Shadow Hills Mastering Compressor | attack 30 ms; release 0.1 s (the instantiate pair only) | attack 0.1–10 ms (+2.58..+5.95); release 0.25 s–Dual (+0.93..+3.36) |
| Shadow Hills Class A Mastering Comp | attack 30 ms; release 0.1 s (instantiate only) | attack 0.1–10 ms (+1.54..+3.51); release 0.25 s–Dual (+0.55..+2.07) |
| Solid Bus Comp | attack 30 ms; release 0.1 s (instantiate only) | attack 0.1–10 ms (+1.30..+3.23); release 0.2 s–Auto (+0.52..+2.42) |
| Lindell 7X-500 | attack Slow; release Fast (instantiate only) | attack Fast +3.41, Medium +2.13; release Medium +1.99, Slow +3.20 |

The same rule applied to the rest of the 37 (full list in `timing_positions_over_0p5.txt`):
- **No timing position refused:** API-2500, Bettermaker, CL 1B, H-Comp, kHs, Lindell SBC, MCompressor, RCompressor, UnFairchild, VComp, U73b and Zip.
- **Some positions refused:** C1 comp, DPR-402, mpressor, Mike-E, DSM V3, SSLComp, SSL Native Bus Compressor 2 (release 0.6–1.2, +0.51..+0.57), TBTECH, Lindell 254E (release 100 mS) and bx_townhouse.

**What the profile carries.** Each time-draft position gets `gr_shift_db` plus `writable_v1: |gr_shift_db| <= 0.5`. The server never writes a position marked `false`. If a user asks for one, the server says the timing would change the amount by X dB, and refuses.

### 2.4 make-up / output misses: the cause (measured) and the fix

**The cause.** The combined prediction is `out = L + static_gain_db − GR + (make-up gives − make-up at neutral)`. `static_gain_db` is ONE number: the quiet gain at the instantiate position.

On units whose amount control is an input or drive gain, the quiet gain moves with the amount pick. The profile already records this in `level_coupling.gain_db_per_point`.

Re-predicting with `level_coupling` interpolated at the combined run's own pick (`quiet_gain_at_pick.txt`):

| unit | old miss | with level_coupling |
|---|---|---|
| VComp | +16.60 | −0.08 |
| Abbey Road RS124 | +22.27 | +0.18 |
| XLA-3 | +16.63 | −0.05 |
| U73b | +10.86 | +0.02 |
| SSL Blitzer | +11.10 | −0.03 |
| MV2 | +10.15 | +0.01 |
| XTComp | +8.80 | +0.12 |
| SSL LMC+ | +6.70 | −0.03 |
| Lindell 7X-500 | +4.24 | −0.06 |
| Mike-E | −1.44 | −0.09 |
| CLA-76 | +12.53 | +1.34 |

That is 10 of 11 within 0.2 dB. The GR itself was never wrong, because GR is measured against the run's own quiet gain.

**The fix, in two places:**
- In the combined pass (`EjmapCombined.h`): predict the static term as `level_coupling` at the pick whenever the coupling's control is the amount control, and keep `static_gain_db` only for uncoupled units.
- On the server: never use `static_gain_db` for an output or gain-match prediction on a coupled unit; read `level_coupling` at the pick instead. As a cheap guard, the export could rename `static_gain_db` to `static_gain_db_at_instantiate`.

**MV2 (target +4 gives 0).** MV2's Output Gain runs −48..0 dB (`gain_profile.draft`: the readings end at 0.0 at norm 1.0). It can only cut. The pass wrote the end and noted it. The fix: when the make-up curve cannot reach the target, v1 refuses the make-up and says how much it can give (0 dB here). It must not write the end silently. The output miss itself (+10.15) is the `level_coupling` cause above.

**SPL IRON.** It has two separate defects:
1. Only `Output L` was written (6 dB, a stepped control, 2..12 dB). `Output R` stayed at its 2 dB neutral. The measured quiet gain, 1.85 dB, equals the static 1.81 dB, so the L write had no effect on what the probe measured. This is the linked-twin class, already the reason IRON is held. The linked-twin fix must also cover the make-up / output controls: write `Output R` to the same position.
2. +5.23 was asked and 6.10 written. The note says "past the curve's span", which is wrong for a value inside 2..12. The stepped inversion reports the nearest step as clamped. That wording is a minor defect, separate from the miss.

**Left unexplained by this data.**
- H-Comp: its quiet gain at the pick is 2.4 dB under `static_gain_db`, and there is no `level_coupling`.
- Vertigo VSC-2: the make-up had no effect at all (quiet 0.02 = static).
- Ozone 12 Vintage: −2.30.
- Kramer PIE: +0.60.

Only H-Comp is in the 37.
