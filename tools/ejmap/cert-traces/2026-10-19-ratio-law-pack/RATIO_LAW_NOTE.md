# The ratio law: a note for Sean (and B), 19 Oct, after the audit and the C1 control

**Your reply (thank you):** the server uses our measured in_at_gr curve at the measured setting and never the label there. The ratio maths at other settings will use our effective ratios once we supply them. So the open question is what to supply for the other settings.

## The short answer
- **The profile's `measured_ratio` is not a true ratio.** It is a median slope that includes the knee. Don't use it as R.
- **The label is not a safe R either, at least on the C1.** It was measured on Kathy's Mac (below): the C1's slope crosses its label, overshoots it, then falls away. It never holds a constant ratio at any depth.
- **So for a unit like the C1, no single number per setting works,** whether the label, the median, or an "effective" fit. What works is the measured curve at each setting, as you already use at the measured one. We propose supplying that (below). Nothing is changed or sent until you agree the shape.
- An earlier draft said the CL 1B's label is wrong. A later draft said "labels hold above the knee". **Neither was sent, and the C1 measurement disproves the second as a general rule.**

## How the two profile figures are computed
**`ratio.curve[0].measured_ratio`** (EjmapSweep.h, "level dependence"):
- It is 1 / (1 - the MEDIAN slope dGR/dInput).
- The slope is taken over every adjacent 2 dB pair of test levels (-60 to 0 dBFS peak), at every threshold position, wherever both readings sit in 0.5-12 dB of GR.
- GR is read after a 2.5 s hold (2.2 s discarded, a 0.3 s window, steady to 0.1 dB), at the ratio the sweep ran at, with the profile's neutral writes.
- **There is no knee handling.**

**`fit.model`'s ratio** (EjmapProfileExport.h):
- A minimax grid search, R in {1.5 … 20} and knee in {0 … 12 dB}, over every in-band reading.
- The CL 1B's **1.5** is the grid floor, with a worst error of 2.75 dB. It is a shape check, not a ratio. The CL 1B's **1.74** is the median slope above, through the knee.

## The audit (your cert 6 records, no loads): slope by GR depth, as a ratio
| plugin (label) | 0.5-2 dB | 2-4 | 4-6 | 6-9 | 9-12 | beyond 12 | measured_ratio |
|---|---|---|---|---|---|---|---|
| CL 1B (6:1) | 1.14 | 1.71 | 2.22 | 2.70 | 3.36 | 6.06 | 1.74 |
| C1 comp (m) (4.13:1) | 1.31 | 1.80 | 2.53 | 3.57 | 5.06 | 2.82 | 2.11 |
| MCompressor (1.8:1) | 2.86 | 2.82 | 2.25 | 2.11 | 1.80 | 1.31 | 2.17 |
| X-Comp v6 (4:1) | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 | 4.00 |

Hard-knee designs (X-Comp, Zip) match their label at every depth; the others don't hold one ratio.

## The C1 control (Kathy's Mac, 19 Oct)
- **Your 15.0.70 profile could not be used.** This Mac has C1 12.0.0, and the version gate refused before any load.
- **What ran instead:** a labelled 12.0.0 control, never exported.
  - The controls are identical across the two versions.
  - Your profile's other-control values were written and read back: Attack 2.00, Release 50, PDR Tc 0, Makeup 0, Output 0, Low Ref.
  - The ratio dial was placed from its own text (4.13:1 read back at the same norm as your profile).
  - Inputs T+3 … T+42 dB, settled readings, at two threshold positions.

| setting (read back) | local ratio at 2 / 6 / 9 / 13 / 18 / 22 / 26 dB GR |
|---|---|
| 4.13:1 | 1.49 / 3.09 / 4.17 / **5.77** / 4.48 / 2.75 / 1.86 |
| 4.00:1 | 1.49 / 3.06 / 4.05 / **5.45** / 4.17 / 2.63 / 1.82 |
| 2.00:1 | 1.39 / 2.10 / 2.19 / 1.57 (peak **2.22** at 7 dB; GR flattens near 16 dB) |
| 48.57:1 | 1.52 / 3.53 / 5.56 / 10-19 / limiting from about 20 dB |

- **The same at both threshold positions,** so it is the plugin's static curve.
- **The law with the label misses by up to 5.3 dB.** With the best-fit effective ratio it still misses by 2.6-4.0 dB.
- **The audit's 5.06 at 9-12 dB is this overshoot.** It is the plugin: not dial placement (the read-back is exact) and not the knee alone.
- **"Hard-knee pass":** the C1 has no knee control. Its Low/Peak Ref switch moved the output by a constant +28.45 dB with identical GR, so it is a level reference.
- **Not measured:** the CL 1B and MCompressor (licence-bound here). Their "label reached deep" readings may be the same overshoot shape. Unknown until they are measured on your Mac.

## The 27 adjustable profiles, from the readings on disk (at the measured setting only)
Median local ratio per GR band (2-6 / 6-9 / 9-12 / beyond 12 dB), from each record's raw sweep:
- **Constant, and equal to the label (4):** kHs Compressor (2.0 everywhere), SSL Native X-Comp v6 (4.0), Unfiltered Audio Zip (4.17), elysia alpha mix (1.36-1.47 for a 1.5 label; only two bands reached).
- **Constant, but not the label (4):**
  - API-2500 m/s (1.7 for a "4:1" setting);
  - Lindell SBC (1.6-1.8 for "4");
  - VBC FG-Red (4.65-4.88 for "4").

  The readings are steady across depth, so the gap is not the knee. Why they differ from the label was not investigated.
- **Varies with depth (19):** the six C1 variants, RCompressor m/s (the same overshoot-then-fall shape as C1), DPR-402, Bettermaker, bx_townhouse, Lindell 254E, MCompressor, Solid Bus Comp, SSL Native Bus Compressor 2, X-ValveComp, SSLComp m/s, CL 1B.

**What this means for the profiles:**
- **Nothing needs re-deriving for the measured setting:** the server already uses the measured curve there.
- **At other settings, all 27 need the per-setting measurement** (the pack), because the data on disk holds one setting only. For the 4 "constant = label" profiles the label is a reasonable prior, not a measurement.
- So no v3d: re-deriving `measured_ratio` from disk would not change what the server does.

## What we propose to supply (your call)
- **Per tested ratio setting, the measured curve:** input at g dB of GR (`in_at_gr_dbfs`), from the knee upward, as you already have at the measured setting. The ratio law run collects it (the deep inputs need `--levels-above`).
- **The `measured_ratios` table stays as a summary.** `use_effective` is false for the C1 at every setting, which correctly tells the server not to use a scalar there.
- **We have NOT changed `measured_ratio` in any profile and made no v3d.** The fix Kathy asked for assumed the C1 would settle at its label above the knee. It doesn't, so a "better" single ratio would mislead in the same way.

## The check (RATIO_LAW_PACK)
- **Settings:** the lowest, 2:1, 4:1 and the highest (the measured setting skipped), at the tone check's g 2 and g 4 positions, inputs T+6/9/12. Points under half a knee above T are left out.
- **Non-linear or named ratio dials** (C1, DPR-402) are placed from the plugin's own text, one extra load after the licence gate.
- **Lists** (B's 27 adjustable; the 18 fixed-ratio are out of scope):
  - `products_sean_mac.txt`: 20: the 7 licence-bound on Kathy's Mac (MCompressor, CL 1B, VBC FG-Red: PACE; the three SSL Native; kHs Compressor) and the 13 Waves profiles (API-2500 m/s, DPR-402, the six C1 variants, SSLComp m/s, RCompressor m/s), served at 15.0.70 while Kathy's Mac has 12.0.0. Dry run: 20 of 20 planned.
  - `products_kathy_mac.txt`: 7 whose installed version equals the served profile's (Lindell 254E, Lindell SBC, Bettermaker, Solid Bus Comp, elysia alpha mix, bx_townhouse, Zip). Dry run: 7 of 7 planned. Kathy runs these.

## The profile field (proposed): `ratio_law`
```json
"ratio_law": {
  "status": "verified" | "partial" | "none",
  "tested_ratios": [2, 4, 10],
  "safe_ratios": [2, 4],
  "safe_between": [2, 4],
  "worst_db": 0.31,
  "knee_db": 6.0,
  "measured_ratio": 6.0,
  "effective_ratio": {"2.00": 2.02, "4.00": 3.96, "10.00": 6.1},
  "threshold_shift_db": {"2.00": 0.1, "4.00": 0.0, "10.00": 1.4},
  "measured_ratios": [{"labelled": 10, "effective": 6.1, "worst_db_label": 2.9, "worst_db_effective": 0.3, "use_effective": true, "points": 6}],
  "rule": "GR = (input - T) x (1 - 1/ratio), T from the measured sweep; pass within 0.5 dB at every point from the knee upward; never past the lowest or highest ratio tested"
}
```
The numbers above are an example.

- **Without the block:** the maths only at the measured setting, from the measured curve, as you do now.
- **`lib/compProfile.js` must accept the field before we send a profile carrying it.** Nothing has been sent.
