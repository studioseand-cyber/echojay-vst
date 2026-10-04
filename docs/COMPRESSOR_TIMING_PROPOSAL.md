# Compressor timing — PROPOSAL (roadmap 2.3), prototype measured 5 Oct 2026

Status: PROTOTYPE, Phase B2 of the 4/5 Oct overnight run. Nothing exported, nothing published; the mode is
`ejmap --cert-timing "<product>" [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]`, reads the product's certified record in
`<cert dir>/fixtures`, and writes `<cert dir>/timing/<identity>.timing.json` (`ej_timing_prototype/0`). The probe gained
`--burst` (probe_burst.h); the derivation is `EjmapTiming.h` (pins K1–K9, mutants M34–M38 red). Seven units measured here, ~10 s each.

## Method

**Burst.** From the record: the amount position whose 1 dB point (peak) is nearest −30 dBFS; quiet = that point − 6 dB, loud = + 10 dB
(capped at −3). A 997 Hz sine: 1 s at quiet, 2 s at loud (4 s post for release controls), back to quiet; the record's own writes
(neutral, engage, ratio raise) plus the amount position; one control under test per burst. The probe prints input and output RMS per
5 ms window **on the input's clock** — the output is aligned by the plugin's reported latency (SBC: 52 samples), because without it
the window straddling a step read a +16 dB spike and "release 1.2 ms" at every position. The window straddling a step is skipped anyway.

**Derivation.** gain(t) = out − in. Baseline = median gain over the second half of pre; final = median over the last 30 % of loud;
GR step = baseline − final. **Attack** = time after the step up at which the gain has dropped 63 % of the step (one time constant for
an exponential; interpolated between windows). **Release** = time after the step down at which the gain has recovered 63 % of the
way back. Guards, each refusing by name: step < 2 dB; loud segment not settled (last-30 % IQR > 0.3 dB: hold too short or the unit
keeps moving); baseline moving. An attack inside the first window is a **bound** ("faster than ~6 ms"); a release not recovered by the
end of post is a **bound** ("longer than 4 s"). **Program dependence**: the release at the instantiate position after a 0.3 s burst
against a 3 s one; a ratio over 2 (or under ½) flags the unit.

**Positions.** Declared detents; else the control is written at 33 norms and its distinct read-backs are the positions when it snaps
(7X-500's Fast / Medium / Slow, 254E's 100mS / 400mS / 800mS / 1.5S / Auto — labels with digits that the "words" test missed); a
true continuous control gets eight evenly spaced positions.

## Results (this Mac, 5 Oct)

### bx_opto 1.10.1 — amount `Peak Reduction` at norm 0.667 (1 dB point -30.80 dBFS peak), burst -36.8 → -20.8
- program dependence: release after a 0.3 s burst 592.2 ms, after 3 s 592.4 ms → ratio 1.0 → **not program-dependent**

### C1 comp (s) 12.0.0 — amount `Threshold` at norm 0.733 (1 dB point -31.00 dBFS peak), burst -37.0 → -21.0
- **Attack** (attack, positions by ?): 0.01 → <8; 0.05 → <8; 0.27 → <8; 1.39 → <8; 7.20 → 9; 37.28 → 42; 193.07 → 219; 1000.00 → 799 ms
- **Release** (release, positions by ?): 1 → 4; 4 → 4; 14 → 12; 52 → 42; 193 → 153; 720 → 566; 2683 → 2113; 10000 → >3998 ms
- program dependence: release after a 0.3 s burst 39.9 ms, after 3 s 39.9 ms → ratio 1.0 → **not program-dependent**

### elysia mpressor 1.15.1 — amount `Threshold` at norm 0.667 (1 dB point -29.98 dBFS peak), burst -36.0 → -20.0
- **Attack** (attack, positions by ?): 0.0 ms → <7; 3.3 ms → <7; 9.6 ms → <7; 15.9 ms → 8; 26.4 ms → 13; 65.7 ms → 30; 117.1 ms → 49; 150.0 ms → 60 ms
- **Release** (release, positions by ?): 5.0 ms → 3; 9.9 ms → 7; 29.4 ms → 25; 75.0 ms → 70; 175.7 ms → 173; 360.0 ms → 364; 741.4 ms → 764; 1200.0 ms → 1250 ms
- program dependence: release after a 0.3 s burst 112.9 ms, after 3 s 113.0 ms → ratio 1.0 → **not program-dependent**

### RCompressor (s) 12.0.0 — amount `Threshold` at norm 0.600 (1 dB point -30.01 dBFS peak), burst -36.0 → -20.0
- **Attack** (attack, positions by even8): 0.50 → <6; 1.34 → <6; 3.60 → <6; 9.65 → 11; 25.9 → 31; 69.5 → 83; 186 → 223; 500 → 557 ms
- **Release** (release, positions by even8): 5.00 → 3; 13.4 → 9; 36.0 → 21; 96.5 → 52; 259 → 124; 695 → 289; 1864 → 694; 5000 → 1752 ms
- program dependence: release after a 0.3 s burst 63.9 ms, after 3 s 81.9 ms → ratio 1.28 → **not program-dependent**

### Lindell 7X-500 1.2.2 — amount `Input` at norm 0.667 (1 dB point -29.10 dBFS peak), burst -35.1 → -19.1
- **Attack** (attack, positions by text): Fast → <8; Medium → <8; Slow → 12 ms
- **Release** (release, positions by text): Fast → 13; Medium → 52; Slow → 231 ms
- **Continuous Attack** (attack, positions by even8): 0.0 → 12; 1.4 → 12; 2.9 → 12; 4.3 → 12; 5.7 → 12; 7.1 → 12; 8.6 → 12; 10.0 → 12 ms
- **Continuous Release** (release, positions by even8): 0.0 → 13; 1.4 → 13; 2.9 → 13; 4.3 → 13; 5.7 → 13; 7.1 → 13; 8.6 → 13; 10.0 → 13 ms
- program dependence: release after a 0.3 s burst 11.2 ms, after 3 s 12.8 ms → ratio 1.14 → **not program-dependent**

### Lindell 254E 1.2.2 — amount `Threshold` at norm 0.333 (1 dB point -29.24 dBFS peak), burst -35.2 → -19.2
- **Compress Recovery** (release, positions by landing): 100mS → 164; 400mS → 740; 800mS → 1535; 1.5S → 3339; Auto → >3998 ms
- **Limit Recovery** (release, positions by landing): 100mS → >3998; 200mS → >3998; 800mS → >3998; Auto → >3998 ms
- program dependence: release after a 0.3 s burst 2773.8 ms, after 3 s >3998.0 ms → ratio – → **not decided**

### Lindell SBC 1.0.3 — amount `Threshold` at norm 0.867 (1 dB point -29.28 dBFS peak), burst -35.3 → -19.3
- **Release** (release, positions by ?): 0.050 → 27; 0.121 → 39; 0.193 → 50; 0.321 → 82; 0.679 → 208; 1.179 → 434; 1.929 → 800; 3.000 → 2066 ms
- **Attack** (attack, positions by ?): 0.03 ms → <6; 0.1 ms → <6; 0.3 ms → <6; 1 ms → 9; 3 ms → 14; 10 ms → 20; 30 ms → 48 ms
- program dependence: release after a 0.3 s burst 60.8 ms, after 3 s 157.3 ms → ratio 2.59 → **PROGRAM-DEPENDENT**


What this says:
- **Labels mostly track, with a unit's own definition**: RCompressor Attack 9.65 → 11 ms, 25.9 → 31, 69.5 → 83, 186 → 223, 500 → 557
  (≈1.15× the label); its Release 36 → 21, 259 → 124, 1864 → 694, 5000 → 1753 (≈0.35–0.6×: the label is not the 63 % time).
  C1 comp Attack 7.2 → 9.4, 37 → 42, 193 → 219, 1000 → 799; Release 52 → 42, 720 → 566, 2683 → 2113 (≈0.79×), 10000 → > 4 s.
  mpressor Release 29.4 → 25, 75 → 70, 176 → 173, 360 → 364, 741 → 764, 1200 → 1250 (**the one unit whose release label IS the
  63 % time**); its Attack 15.9 → 8, 26.4 → 13, 65.7 → 30, 117 → 49, 150 → 60 (≈0.4–0.5×). SBC Release 0.05 s → 27 ms …
  3.0 s → 2066 ms (≈0.55–0.7×); Attack 1 → 9, 3 → 14, 10 → 20, 30 → 48 ms. 254E Compress Recovery 100mS → 164, 400 → 740,
  800 → 1535, 1.5S → 3339 (≈1.6–2.2×: "recovery" there is to full, not to 63 %); Auto > 4 s.
- **Attacks under ~6 ms are bounds** at this window (5 ms RMS windows on a 997 Hz tone: ~5 cycles). A 1 ms window on a 4 kHz tone
  would resolve 1–2 ms attacks; proposed as a second pass for units whose attack labels run below 5 ms.
- **Program dependence found**: Lindell SBC (61 ms after 0.3 s, 157 ms after 3 s, ratio 2.6). Not found on bx_opto (592 ms both — a
  fixed opto release at this setting), mpressor, C1 comp, RComp (1.28), 7X-500 (1.14). 254E not decided (Auto > 4 s).
- **The GR step itself moves with the attack** on mpressor (8.1 dB at Attack 0 → 6.0 dB at 150 ms) and 7X-500 (13.0 Fast → 9.6 Slow):
  a slower attack lets more of the 2 s hold pass before full GR, and the last-30 % median still sits below full. Hold should scale
  with the attack label (≥ 10 attack times), or the final be read from the longest hold that settles.
- 7X-500's Continuous Attack/Release do nothing at these settings (the switched Attack/Release are active): no_effect, listed.

## Draft `time` field (COMP_PROFILE_SPEC)

```json
"time": {
  "attack": { "control": "Attack", "positions": [ { "norm": 0.0, "display": "0.03 ms", "attack_ms": null, "faster_than_ms": 6.4 },
                                                   { "norm": 0.43, "display": "1 ms", "attack_ms": 8.8 }, "…" ] },
  "release": { "control": "Release", "positions": [ { "norm": 1.0, "display": "3.000", "release_ms": 2066 }, "…" ] },
  "program_dependent": true, "release_ratio_long_over_short": 2.59,
  "method": "997 Hz burst 10 dB above the position's 1 dB point; attack = 63 % of the GR step, release = 63 % recovery; 5 ms windows"
}
```
As measured, never as labelled; null with a bound where the window or the post segment limited it; the amount position the
timing was measured at stated.

## Questions for Sean

1. Is 63 % (one time constant) the definition the server wants, or 90 % / full? The unit labels disagree among themselves
   (mpressor's release label is the 63 % time; 254E's "recovery" is ~2× it; RComp's ~0.5×), so the field must say its definition.
2. Attack resolution: is a 5 ms bound enough for the feel ladder, or should fast attacks get the 1 ms / 4 kHz pass?
3. Where the GR step depends on the attack setting (mpressor), does `time` carry the step it was measured at?
4. Program-dependent units: one flag, or the two release times (short / long burst) themselves?
