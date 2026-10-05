# Transient shapers and gates — PROPOSAL (roadmap 2.8), prototype 5 Oct 2026 (overnight run 2, R5)

Status: PROTOTYPE. Nothing exported, nothing published. Mode: `ejmap --cert-dynamics "<product>" [--kind transient|gate]
[--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]`, writing `<cert dir>/dynamics/<identity>.dynamics.json`
(`ej_dynamics_prototype/0`). The kind comes from the ledger's category (`transient_shaper` / `gate`) unless said.

## The measurement

The probe gained two modes (`probe_dynamics.h`): **`--hits`**, a drum-like burst — a 997 Hz sine whose envelope rises in one
sample to −6 dBFS and falls exponentially (150 ms time constant), four hits 600 ms apart after a settling second; per 1 ms
window the input's and output's RMS and peak on the input's clock (latency taken out). **`--ramp`**, a 997 Hz sine rising
linearly in dB from −70 to −6 dBFS over 3 s and falling back over 3 s; per 5 ms window the RMS in and out. Gate timing reuses
`--burst` (quiet / loud / quiet, 1 ms windows). `EjmapDynamics.h` derives, pure:

- **Transient shapers.** Per hit: transient = output peak over input peak in the first 10 ms; sustain = output RMS over input
  RMS at 80–250 ms; medians over hits 2–4 (one hit settles a detector). A control position's effect = its figures minus the
  neutral run's (the unit as instantiated); a dB label is compared within 1 dB.
- **Gates.** From the ramp: gain = out − in (a closed gate's digital silence read as −150 dBFS, never skipped); closed gain =
  the median over the quietest tenth of the way up, open gain = the loudest tenth; range = open − closed (gating when ≥ 3 dB);
  open level = the input where the gain crosses midway on the way up, close level = where it crosses back on the way down;
  hysteresis = open − close; peak = RMS + 3.01 for the sine, compared with a dB label. From the burst (threshold at norm 0.5,
  quiet 12 dB under the measured open level, loud 12 dB over): attack = time to within 1 dB of open; hold = time after the
  step down the gain stays within 1 dB of open; release = from there to 20 dB below open (or to 1 dB of closed when the range
  is smaller), interpolated between windows.

Pins H1–H5, G1–G4, L1 (RoundTripTest).

## Measured on this Mac (5 Oct; `cert-traces/2026-10-05-dynamics/`)

### Transient shapers

| product | attack control | sustain control | finding |
|---|---|---|---|
| Smack Attack (s) | Attack ±100 → transient **−24 / −12 / 0 / +12 / +24 dB**, exactly linear (0.24 dB per unit) | Sustain ±100 → −6.95 / −5.78 / 0 / +4.32 / +7.69 dB | the cleanest unit: attack and sustain fully separate (sustain moved by 0.3 dB at most under Attack) |
| MTransient | Transient → Attack labels −24..+24 dB → **+12 / +24 within 0.03 dB** of the label; −12 / −24 read −5.75 / −6.92 (the cut saturates at ~−7 on this hit) | Basic − Sustain ±24 dB label → ±2.9 dB measured | the sustain lane barely moves a 150 ms-decay hit: the sustain window (80–250 ms) sits in the hit's own decay — a longer hit is needed to read sustain labels (below) |
| elysia nvelope | Attack/Gain H ±15 dB label → −7.2 / −6.3 / 0 / +8.3 / +13.9 dB (the top within 1.1, the rest off by 2.6–7.8) | Sustain/Gain L ±15 → ±0.4 dB (nothing) | the "H" / "L" are frequency-split gains (nvelope's dual-band mode): the sustain lane acts on the low band, which a 997 Hz hit does not reach — the test signal, not the unit |
| Transient Master | Attack ±100 % → −12.9 / −6.7 / 0 / +6.0 / +6.0 dB (clips at +6 above 50 %) | Sustain ±100 % → −5.4 / −2.8 / 0 / +3.0 / +6.2 dB | the sustain control also moves the transient (+3.2 dB at −100 %): the two are coupled |
| TransX Wide (m), TransX Multi (s) | no "Attack" by name — TransX's amount is **Range** (missed by the name rule); only Release matched | — | neutral already boosts the transient by +5.1 / +4.3 dB (the instantiate state is not neutral) |
| Quantum | the name rule took "Attack − Vibrato − Mix" and "Sustain − Phaser − Feedback" (an envelope-modulation effect, not a shaper) | | 0.00 dB everywhere: not a transient shaper's controls |
| spiff | no attack / sustain controls (cut depth / boost depth / sensitivity) | | the name rule needs the product's own words |

### Gates

| product | threshold (open level vs label) | hysteresis | range | attack / hold / release vs label |
|---|---|---|---|---|
| Unfiltered Audio G8 | **−60 / −40 / −20 dB labels open at −60.0 / −40.0 / −20.0 dBFS peak (0.0 dB off)** | 6.5 dB at every setting | Reduction labels **exact** (0 / −13.7 / −∞ → −80 floor) | attack 1 ms → 1.2, 126 → 71.7, 500 → 284 (ratio 0.57: the label is the full rise, the figure is to 1 dB of open); **hold 125 → 129.6, 500 → 504.5 (1.01–1.04×)**; release 1 → 1.2, 501 → 238, 2000 → 525 for 11.5 dB (the −20 dB point is reached at ~0.47 × the label: the label is the full fall) |
| C1 gate (s) | Gate Open −50 / −25 → opens at −49.1 / −24.1 dBFS peak (**0.9 dB above the label**) | 2.9–3.0 dB | Floor −10 N → −80 (floor), −5.7 → −5.7, 12.0 → +12.0 | attack 3.16 → 7.3 ms (2.3×); hold 7.07 → 18.7 (hold reads the same at 0.01: the unit's minimum hold ~19 ms); release 1 → 2.2, 100 → 219 (2.2× for 20 dB); 1000 ms attack never opened the burst (1 s hold) and 5 s hold / 10 s release never began to close in the 3 s post |
| PSE (m) | not gating at any threshold on the ramp (0.00 dB range) | — | Range −60 / −30 / 0 → closed −33.4 / −30.0 / 0 (the range control attenuates on its own) | — |

What the numbers say:
- **G8's labels are honest to 0.0 dB on threshold and range**, and its hold to 4 %; attack and release labels are full-rise /
  full-fall times, so the 1 dB / 20 dB figures read 0.5–0.6× — a definition difference, said, not an error.
- **C1 gate opens ~1 dB above its label** at both settings; hysteresis 3 dB; its minimum hold is ~19 ms whatever the label.
- **The hit is too short for sustain**: a 150 ms decay leaves the 80–250 ms window inside the hit's own fall; MTransient's ±24 dB
  sustain lane reads ±2.9. A 500 ms decay (or a held tone with a transient on it) is the next step for sustain labels.
- **The name rule again**: TransX's amount is "Range", spiff has its own words, Quantum matched on modulation names. The
  measurement can decide (a control that moves the transient figure by more than 1 dB across its positions is an attack
  control; one that moves the sustain figure is a sustain control) — a second pass over every numeric control.
- The burst for gate timing uses one threshold position (norm 0.5): a unit whose norm 0.5 is an extreme threshold (C1 gate at −50)
  gets a quiet level clamped at −80 and loud at −38 — said in the record; the threshold should come from the ramp's reading.

## Questions for Sean

1. Transient profile: ±dB of transient and sustain per position (what the server would write for "more punch"), or a feel ladder?
2. Gate profile: open level in peak dBFS against the label, hysteresis, range — and which timing definition (to 1 dB / 20 dB, or
   the vendor's full rise / fall) the server wants.
