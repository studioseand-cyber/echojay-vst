# Limiters — PROPOSAL (roadmap 2.4), ceiling accuracy prototype measured 5 Oct 2026

Status: PROTOTYPE, Phase B3 of the 4/5 Oct overnight run. Nothing exported, nothing published; the mode is
`ejmap --cert-limiter "<product>" [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]`, writing
`<cert dir>/limiter/<identity>.limiter.json` (`ej_limiter_prototype/0`). The probe's hold line now carries the output's sample
peak and a **4× cubic-interpolated true-peak estimate** (an approximation of BS.1770's oversampled peak, said as such); the
derivation is `EjmapLimiter.h` (pins L1–L9, mutants M39–M41 red). Four limiters measured here; SSL X-Limit is PACE-bound (a
window at load; the watch killed it) and was skipped; bx_limiter (the plain one) has no ceiling control (Threshold / Gain Boost /
XL only) and is listed, not judged.

## Method

A 997 Hz sine at −1 dBFS peak into the limiter. The ceiling control is found by name (ceiling / margin); its labels are read at
33 norms and the positions whose labels come nearest −0.1, −0.3, −1, −3 and −6 dBFS are measured. The amount control is the
sweep's plan (threshold, or input-as-threshold) at its **hard end, decided by measurement**: of the two ends, the one whose output
true peak sits nearest the ceiling label (not the quieter one — bx_limiter True Peak's Input Trim at its minimum is 12 dB of
attenuation, not limiting). Where a two-state oversampling / true-peak switch exists, every position runs with it off and on.
Ceiling error = output peak − label; a limiter **holds** the ceiling when no driven position overshoots by more than 0.1 dB,
separately for the sample peak and the true peak. A position whose output sits more than 0.5 dB BELOW the label was never
pushed into the ceiling ("not driven") and is excluded, said.

The GR ladder and the release are the compressor machinery's: `--cert-sweep` (the threshold ladder, with the limiter's own
amount control) and `--cert-timing` (the burst, roadmap 2.3) run unchanged on a limiter; not repeated here.

## Results (this Mac, 5 Oct)

### bx_limiter True Peak 1.2.1 — ceiling `Ceiling`, amount `Input Trim` at its hard end (norm 1), no oversampling switch

| ceiling label | OS | out sample peak | overshoot | out true peak | overshoot | driven |
|---|---|---|---|---|---|---|
| -0.12 dB | – | -1.02 | -0.90 | -1.02 | -0.90 | no |
| -0.26 dB | – | -1.02 | -0.76 | -1.02 | -0.76 | no |
| -1.05 dB | – | -1.07 | -0.02 | -1.07 | -0.02 | yes |
| -2.93 dB | – | -2.94 | -0.01 | -2.94 | -0.01 | yes |
| -5.74 dB | – | -5.75 | -0.01 | -5.75 | -0.01 | yes |

**verdict**: sample holds, true peak holds — worst sample-peak overshoot -0.01 dB, worst true-peak overshoot -0.01 dB over 3 driven position(s) (bar 0.1); 2 position(s) not driven (the signal never reached that ceiling)


### L2 (s) 12.0.0 — ceiling `Ceiling Slider`, amount `Thresh Slider` at its hard end (norm 0), no oversampling switch

| ceiling label | OS | out sample peak | overshoot | out true peak | overshoot | driven |
|---|---|---|---|---|---|---|
| 0.0 | – | -0.01 | -0.01 | -0.01 | -0.01 | yes |
| -0.9 | – | -0.90 | +0.00 | -0.90 | +0.00 | yes |
| -1.9 | – | -1.90 | +0.00 | -1.90 | +0.00 | yes |
| -2.8 | – | -2.80 | +0.00 | -2.80 | +0.00 | yes |
| -5.6 | – | -5.60 | +0.00 | -5.60 | +0.00 | yes |

**verdict**: sample holds, true peak holds — worst sample-peak overshoot 0.00 dB, worst true-peak overshoot 0.00 dB over 5 driven position(s) (bar 0.1)


### Newfangled Elevate 1.12.7 — ceiling `Ceiling`, amount `Input Level` at its hard end (norm 1), oversampling `True Peak`

| ceiling label | OS | out sample peak | overshoot | out true peak | overshoot | driven |
|---|---|---|---|---|---|---|
| -0.13 | off | -0.16 | -0.03 | -0.14 | -0.01 | yes |
| -0.26 | off | -0.29 | -0.03 | -0.28 | -0.02 | yes |
| -1.00 | off | -1.03 | -0.03 | -1.01 | -0.01 | yes |
| -2.92 | off | -2.95 | -0.03 | -2.94 | -0.02 | yes |
| -5.27 | off | -5.30 | -0.03 | -5.28 | -0.01 | yes |
| -0.13 | on | -0.30 | -0.17 | -0.28 | -0.15 | yes |
| -0.26 | on | -0.43 | -0.17 | -0.42 | -0.15 | yes |
| -1.00 | on | -1.17 | -0.17 | -1.15 | -0.15 | yes |
| -2.92 | on | -3.09 | -0.17 | -3.07 | -0.15 | yes |
| -5.27 | on | -5.43 | -0.16 | -5.42 | -0.15 | yes |

**verdict oversampling off**: sample holds, true peak holds — worst sample-peak overshoot -0.03 dB, worst true-peak overshoot -0.01 dB over 5 driven position(s) (bar 0.1)

**verdict oversampling on**: sample holds, true peak holds — worst sample-peak overshoot -0.16 dB, worst true-peak overshoot -0.15 dB over 5 driven position(s) (bar 0.1)


### MLimiterX 14.16.0 — ceiling `Globals -> Ceiling`, amount `Threshold` at its hard end (norm 0), no oversampling switch

| ceiling label | OS | out sample peak | overshoot | out true peak | overshoot | driven |
|---|---|---|---|---|---|---|
| 0.00 dB | – | -0.00 | -0.00 | 0.58 | +0.58 | yes |
| -0.75 dB | – | -0.75 | +0.00 | -0.17 | +0.58 | yes |
| -1.50 dB | – | -1.50 | +0.00 | -0.92 | +0.58 | yes |
| -3.00 dB | – | -3.00 | +0.00 | -2.42 | +0.58 | yes |
| -6.00 dB | – | -6.00 | +0.00 | -5.42 | +0.58 | yes |

**verdict**: sample holds, true peak OVER — worst sample-peak overshoot -0.00 dB, worst true-peak overshoot 0.58 dB over 5 driven position(s) (bar 0.1)


What this says:
- **MLimiterX holds the sample peak to 0.00 dB and overshoots TRUE peak by +0.58 dB at every ceiling** — a sample-peak limiter
  at these settings (Melda's own true-peak option, if it has one here, is not a two-state switch our name rule found). The one
  real catch of the pass: a −1 dBTP master out of it would read −0.42 dBTP.
- **L2 holds both to 0.00 dB** (Waves' L2 is a true-peak-safe design at this drive); **bx_limiter True Peak holds both within
  0.02 dB** on its driven ceilings; **Newfangled Elevate holds both**, and its True Peak switch moves the output 0.14 dB further
  under the label (headroom, not overshoot).
- The drive must exceed the ceiling: bx TP's output at −1.02 dBFS never reached its −0.12 / −0.26 ceilings (its Input Trim tops out
  at 0 dB) — those positions are "not driven", not "holds". A hotter drive (0 dBFS, or the amount control's own range) is the fix.
- True-peak estimate: 4× cubic interpolation. On a 997 Hz sine the inter-sample peak is at most 0.003 dB above the sample peak at
  48 kHz, so the +0.58 dB on MLimiterX is real behaviour, not estimator error; for a full BS.1770 figure the probe should
  upsample with the standard's FIR (a small addition).

## Draft limiter profile (for Sean)

```json
"limiter": {
  "ceiling": { "control": "Ceiling", "holds_sample_peak": true, "holds_true_peak": false, "worst_true_overshoot_db": 0.58,
               "positions": [ { "norm": 1.0, "display": "0.00 dB", "label_db": 0.0, "out_true_peak_db": 0.58 }, "…" ],
               "oversampling": { "control": "True Peak", "on_moves_output_db": -0.14 } },
  "gr_curve": "the compressor ladder on the limiter's amount control (ej_comp_profile amount.curve, unchanged)",
  "release": "the burst's release_ms per release position (roadmap 2.3)"
}
```

## Questions for Sean

1. Is the ceiling judged on the true peak (BS.1770) or the sample peak? MLimiterX passes one and fails the other.
2. Drive level: −1 dBFS as here, 0 dBFS, or the amount control's full range? Ceilings above the drive cannot be tested.
3. Does the server write the oversampling switch on when the unit has one (Elevate: −0.14 dB of extra headroom)?
4. Release: reuse `time.release_ms` from the burst as is, or does a limiter need the release at its own (much faster) scale?
