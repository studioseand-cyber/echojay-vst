# EQ response — PROPOSAL (roadmap 2.2), prototype measured 5 Oct 2026

Status: PROTOTYPE, Phase B4 of the 4/5 Oct overnight run. Nothing exported, nothing published; the mode is
`ejmap --cert-eq "<product>" [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]`, writing `<cert dir>/eq/<identity>.eq.json`
(`ej_eq_prototype/0`). The probe gained `--response` (probe_response.h: a multitone with exact Goertzel bins); the derivation and
the band grouping are `EjmapEq.h` (pins E1–E9, mutants M42–M45 red). Five EQs measured here in 3m28 — two digital (bx_digital V3,
bx_2098), two analog-style (Lindell PEX-500 Pultec, AMEK EQ 200), one shelving (Dangerous BAX).

## Method

**Signal.** 121 sines log-spaced 20 Hz–20 kHz (1/12 octave), random fixed phases, the sum at −12 dBFS peak, each tone snapped to
a whole number of cycles over the 0.5 s measured span so its Goertzel bin is exact; 1 s hold, 0.5 s discarded. The output's
magnitude at every tone against the input's, per position.

**Bands.** Plan-time, by name token: gain (gain / boost / cut / atten / level / dB), frequency (freq / frequency / Hz / kHz),
Q (q / width / bandwidth / bw / shape / slope); the band key is the rest of the name ("LF Gain" → "LF"; "Low boost" → "Low");
controls sharing a key form a band when it has a gain and a frequency. Per band: the gain control at 7 norms (freq and Q as
instantiated); the frequency control at 7 norms with the gain at the label nearest +6 dB (else its top); each Q position at that
gain. Every response is read against the band's **baseline** — one response with no write at all (`norms=current`), because a
quick fixture's "instantiate norm" was wrong on bx_digital (0.0 for a "0.0 dB" gain) and put the baseline 12 dB off.

**Derivation.** deviation(f) = (out−in)(f) − baseline. Centre = the tone of largest |deviation|, refined parabolically in log f
(within 1.5 % between tones); gain = the deviation there; bandwidth = the −3 dB span around the peak, interpolated. No −3 dB point on
one side = a **shelf**, with its **corner** where the deviation is half the plateau (the frequency label's meaning for a shelf).
|deviation| under 0.5 dB everywhere = **flat** (the control did nothing at that position). Resolution: a centre to ~3 %, a bandwidth
to ~0.1 octave; a shelf whose plateau is not reached inside 20 Hz–20 kHz reads its plateau low and its corner off (an 8 kHz
first-order shelf: 13 % low) — the grid should extend to 10 Hz–24 kHz at 48 kHz.

## Results (this Mac, 5 Oct)

### bx_2098 EQ 1.8.1
- **HF 1** — gain `HF Gain 1`: -18.0 dB → -18.04 dB plateau (high_shelf, corner 4282 Hz); -11.3 dB → -11.58 dB plateau (high_shelf, corner 3866 Hz); -5.0 dB → -5.08 dB plateau (high_shelf, corner 4263 Hz); 0.0 dB → flat; +4.5 dB → +6.21 dB plateau (high_shelf, corner 4057 Hz); +10.7 dB → +12.87 dB plateau (high_shelf, corner 3756 Hz); +18.0 dB → +18.05 dB plateau (high_shelf, corner 4279 Hz)
  - freq `HF Frequency 1` (gain at 4.5): 2.00k Hz → +6.04 dB plateau (high_shelf, corner 967 Hz); 4.08k Hz → +6.19 dB plateau (high_shelf, corner 1906 Hz); 6.57k Hz → +6.18 dB plateau (high_shelf, corner 2552 Hz); 8.65k Hz → +6.23 dB plateau (high_shelf, corner 3146 Hz); 11.47k Hz → +6.20 dB plateau (high_shelf, corner 4017 Hz); 16.00k Hz → +6.11 dB plateau (high_shelf, corner 5110 Hz); 21.00k Hz → +5.94 dB plateau (high_shelf, corner 6805 Hz)
- **HF 2** — gain `HF Gain 2`: -18.0 dB → -18.04 dB plateau (high_shelf, corner 4282 Hz); -11.3 dB → -11.58 dB plateau (high_shelf, corner 3866 Hz); -5.0 dB → -5.08 dB plateau (high_shelf, corner 4263 Hz); 0.0 dB → flat; +4.5 dB → +6.21 dB plateau (high_shelf, corner 4057 Hz); +10.7 dB → +12.87 dB plateau (high_shelf, corner 3756 Hz); +18.0 dB → +18.05 dB plateau (high_shelf, corner 4279 Hz)
  - freq `HF Frequency 2` (gain at 4.5): 2.00k Hz → +6.04 dB plateau (high_shelf, corner 967 Hz); 4.08k Hz → +6.19 dB plateau (high_shelf, corner 1906 Hz); 6.57k Hz → +6.18 dB plateau (high_shelf, corner 2552 Hz); 8.65k Hz → +6.23 dB plateau (high_shelf, corner 3146 Hz); 11.47k Hz → +6.20 dB plateau (high_shelf, corner 4017 Hz); 16.00k Hz → +6.11 dB plateau (high_shelf, corner 5110 Hz); 21.00k Hz → +5.94 dB plateau (high_shelf, corner 6805 Hz)
- **HMF 1** — gain `HMF Gain 1`: -18.0 dB → -19.09 dB @ 2096 Hz, bw 1.90 oct; -11.3 dB → -10.97 dB @ 2098 Hz, bw 3.07 oct; -5.0 dB → -4.70 dB @ 2106 Hz, bw 5.07 oct; 0.0 dB → flat; +4.5 dB → +4.50 dB @ 2092 Hz, bw 5.25 oct; +10.7 dB → +10.26 dB @ 2093 Hz, bw 3.29 oct; +18.0 dB → +19.09 dB @ 2091 Hz, bw 1.90 oct
  - freq `HMF Frequency 1` (gain at 4.5): 500.0 Hz → +4.50 dB @ 483 Hz, bw 5.56 oct; 808.3 Hz → +4.50 dB @ 1023 Hz, bw 5.48 oct; 1.36k Hz → +4.50 dB @ 1561 Hz, bw 5.38 oct; 1.80k Hz → +4.50 dB @ 2092 Hz, bw 5.25 oct; 2.43k Hz → +4.50 dB @ 2714 Hz, bw 5.11 oct; 3.50k Hz → +4.50 dB @ 3722 Hz, bw 4.87 oct; 4.50k Hz → +4.50 dB @ 4881 Hz, bw 4.59 oct
  - q `HMF Q 1`: 0.65 → bw 5.25 oct; 0.99 → bw 4.81 oct; 1.33 → bw 3.36 oct; 1.66 → bw 3.14 oct; 2.00 → bw 2.87 oct
- **HMF 2** — gain `HMF Gain 2`: -18.0 dB → -19.09 dB @ 2096 Hz, bw 1.90 oct; -11.3 dB → -10.97 dB @ 2098 Hz, bw 3.07 oct; -5.0 dB → -4.70 dB @ 2106 Hz, bw 5.07 oct; 0.0 dB → flat; +4.5 dB → +4.50 dB @ 2092 Hz, bw 5.25 oct; +10.7 dB → +10.26 dB @ 2093 Hz, bw 3.29 oct; +18.0 dB → +19.09 dB @ 2091 Hz, bw 1.90 oct
  - freq `HMF Frequency 2` (gain at 4.5): 500.0 Hz → +4.50 dB @ 483 Hz, bw 5.56 oct; 808.3 Hz → +4.50 dB @ 1023 Hz, bw 5.48 oct; 1.36k Hz → +4.50 dB @ 1561 Hz, bw 5.38 oct; 1.80k Hz → +4.50 dB @ 2092 Hz, bw 5.25 oct; 2.43k Hz → +4.50 dB @ 2714 Hz, bw 5.11 oct; 3.50k Hz → +4.50 dB @ 3722 Hz, bw 4.87 oct; 4.50k Hz → +4.50 dB @ 4881 Hz, bw 4.59 oct
  - q `HMF Q 2`: 0.65 → bw 5.25 oct; 0.99 → bw 4.81 oct; 1.33 → bw 3.36 oct; 1.66 → bw 3.14 oct; 2.00 → bw 2.87 oct
- **LF 1** — gain `LF Gain 1`: -18.0 dB → -19.31 dB plateau (low_shelf, corner 124 Hz); -11.3 dB → -11.00 dB plateau (low_shelf, corner 130 Hz); -5.0 dB → -5.36 dB plateau (low_shelf, corner 142 Hz); 0.0 dB → flat; +4.5 dB → +5.89 dB plateau (low_shelf, corner 147 Hz); +10.7 dB → +13.22 dB plateau (low_shelf, corner 140 Hz); +18.0 dB → +18.99 dB plateau (low_shelf, corner 124 Hz)
  - freq `LF Frequency 1` (gain at 4.5): 30.0 Hz → +5.85 dB plateau (low_shelf, corner 108 Hz); 63.3 Hz → +5.89 dB plateau (low_shelf, corner 149 Hz); 92.0 Hz → +5.99 dB plateau (low_shelf, corner 196 Hz); 111.5 Hz → +6.16 dB plateau (low_shelf, corner 253 Hz); 143.3 Hz → +6.14 dB plateau (low_shelf, corner 315 Hz); 208.3 Hz → +6.15 dB plateau (low_shelf, corner 393 Hz); 300.0 Hz → +6.21 dB plateau (low_shelf, corner 499 Hz)
- **LF 2** — gain `LF Gain 2`: -18.0 dB → -19.31 dB plateau (low_shelf, corner 124 Hz); -11.3 dB → -11.00 dB plateau (low_shelf, corner 130 Hz); -5.0 dB → -5.36 dB plateau (low_shelf, corner 142 Hz); 0.0 dB → flat; +4.5 dB → +5.89 dB plateau (low_shelf, corner 147 Hz); +10.7 dB → +13.22 dB plateau (low_shelf, corner 140 Hz); +18.0 dB → +18.99 dB plateau (low_shelf, corner 124 Hz)
  - freq `LF Frequency 2` (gain at 4.5): 30.0 Hz → +5.85 dB plateau (low_shelf, corner 108 Hz); 63.3 Hz → +5.89 dB plateau (low_shelf, corner 149 Hz); 92.0 Hz → +5.99 dB plateau (low_shelf, corner 196 Hz); 111.5 Hz → +6.16 dB plateau (low_shelf, corner 253 Hz); 143.3 Hz → +6.14 dB plateau (low_shelf, corner 315 Hz); 208.3 Hz → +6.15 dB plateau (low_shelf, corner 393 Hz); 300.0 Hz → +6.21 dB plateau (low_shelf, corner 499 Hz)
- **LMF 1** — gain `LMF Gain 1`: -18.0 dB → -18.62 dB @ 308 Hz, bw 1.87 oct; -11.3 dB → -11.21 dB @ 308 Hz, bw 3.13 oct; -5.0 dB → -5.17 dB @ 309 Hz, bw 5.04 oct; 0.0 dB → flat; +4.5 dB → +4.62 dB @ 309 Hz, bw 5.32 oct; +10.7 dB → +11.07 dB @ 308 Hz, bw 3.04 oct; +18.0 dB → +18.25 dB @ 308 Hz, bw 1.88 oct
  - freq `LMF Frequency 1` (gain at 4.5): 100.0 Hz → +4.62 dB plateau (low_shelf, corner 437 Hz); 154.2 Hz → +4.63 dB @ 157 Hz, bw 5.32 oct; 208.3 Hz → +4.62 dB @ 216 Hz, bw 5.32 oct; 267.5 Hz → +4.62 dB @ 274 Hz, bw 5.32 oct; 360.0 Hz → +4.62 dB @ 355 Hz, bw 5.31 oct; 558.3 Hz → +4.62 dB @ 539 Hz, bw 5.30 oct; 1.00k Hz → +4.62 dB @ 952 Hz, bw 5.26 oct
  - q `LMF Q 1`: 0.65 → bw 5.32 oct; 0.99 → bw 4.71 oct; 1.33 → bw 3.18 oct; 1.66 → bw 2.89 oct; 2.00 → bw 2.65 oct
- **LMF 2** — gain `LMF Gain 2`: -18.0 dB → -18.62 dB @ 308 Hz, bw 1.87 oct; -11.3 dB → -11.21 dB @ 308 Hz, bw 3.13 oct; -5.0 dB → -5.17 dB @ 309 Hz, bw 5.04 oct; 0.0 dB → flat; +4.5 dB → +4.62 dB @ 309 Hz, bw 5.32 oct; +10.7 dB → +11.07 dB @ 308 Hz, bw 3.04 oct; +18.0 dB → +18.25 dB @ 308 Hz, bw 1.88 oct
  - freq `LMF Frequency 2` (gain at 4.5): 100.0 Hz → +4.62 dB plateau (low_shelf, corner 437 Hz); 154.2 Hz → +4.63 dB @ 157 Hz, bw 5.32 oct; 208.3 Hz → +4.62 dB @ 216 Hz, bw 5.32 oct; 267.5 Hz → +4.62 dB @ 274 Hz, bw 5.32 oct; 360.0 Hz → +4.62 dB @ 355 Hz, bw 5.31 oct; 558.3 Hz → +4.62 dB @ 539 Hz, bw 5.30 oct; 1.00k Hz → +4.62 dB @ 952 Hz, bw 5.26 oct
  - q `LMF Q 2`: 0.65 → bw 5.32 oct; 0.99 → bw 4.71 oct; 1.33 → bw 3.18 oct; 1.66 → bw 2.89 oct; 2.00 → bw 2.65 oct

### bx_digital V3 3.10.1
- **EQ Band HF 1** — gain `EQ Band HF 1 Gain`: -12.0 dB → -11.69 dB plateau (high_shelf, corner 8550 Hz); -8.0 dB → -7.71 dB plateau (high_shelf, corner 9413 Hz); -4.0 dB → -3.81 dB plateau (high_shelf, corner 10428 Hz); 0.0 dB → flat; +4.0 dB → +3.81 dB plateau (high_shelf, corner 10429 Hz); +8.0 dB → +7.71 dB plateau (high_shelf, corner 9414 Hz); +12.0 dB → +11.69 dB plateau (high_shelf, corner 8551 Hz)
  - freq `EQ Band HF 1 Frequency` (gain at 4.0): 2.00k Hz → +4.00 dB plateau (high_shelf, corner 1782 Hz); 3.30k Hz → +4.00 dB plateau (high_shelf, corner 2936 Hz); 5.43k Hz → +3.99 dB plateau (high_shelf, corner 4832 Hz); 8.94k Hz → +3.94 dB plateau (high_shelf, corner 7903 Hz); 14.74k Hz → +3.59 dB plateau (high_shelf, corner 12530 Hz); 24.28k Hz → +1.72 dB plateau (high_shelf, corner 17723 Hz); 40.00k Hz → flat
  - q `EQ Band HF 1 Q`: 0.3 → shelf; 0.8 → shelf; 2.0 → bw 1.41 oct; 5.5 → bw 0.57 oct; 15.0 → bw 0.26 oct
- **EQ Band HF 2**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)
- **EQ Band HMF 1** — gain `EQ Band HMF 1 Gain`: -12.0 dB → -15.32 dB @ 5796 Hz, bw 0.14 oct; -8.0 dB → -7.89 dB @ 5800 Hz, bw 0.59 oct; -4.0 dB → -3.92 dB @ 5799 Hz, bw 1.08 oct; 0.0 dB → flat; +4.0 dB → +3.92 dB @ 5799 Hz, bw 1.08 oct; +8.0 dB → +7.89 dB @ 5800 Hz, bw 0.59 oct; +12.0 dB → +11.88 dB @ 5800 Hz, bw 0.52 oct
  - freq `EQ Band HMF 1 Frequency` (gain at 4.0): 400 Hz → +4.00 dB @ 401 Hz, bw 1.04 oct; 780 Hz → +3.96 dB @ 780 Hz, bw 1.06 oct; 1.52k Hz → +3.98 dB @ 1520 Hz, bw 1.05 oct; 2.97k Hz → +3.99 dB @ 2967 Hz, bw 1.04 oct; 5.78k Hz → +3.94 dB @ 5782 Hz, bw 1.07 oct; 11.28k Hz → +4.00 dB @ 11272 Hz, bw 1.00 oct; 22.00k Hz → +3.17 dB plateau (high_shelf, corner 16853 Hz)
  - q `EQ Band HMF 1 Q`: 0.3 → shelf; 0.8 → bw 3.24 oct; 2.0 → bw 1.50 oct; 5.5 → bw 0.69 oct; 15.0 → shelf
- **EQ Band HMF 2**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)
- **EQ Band LF 1** — gain `EQ Band LF 1 Gain`: -12.0 dB → -11.99 dB plateau (low_shelf, corner 141 Hz); -8.0 dB → -8.00 dB plateau (low_shelf, corner 126 Hz); -4.0 dB → -4.00 dB plateau (low_shelf, corner 112 Hz); 0.0 dB → flat; +4.0 dB → +4.00 dB plateau (low_shelf, corner 112 Hz); +8.0 dB → +7.99 dB plateau (low_shelf, corner 126 Hz); +12.0 dB → +11.99 dB plateau (low_shelf, corner 141 Hz)
  - freq `EQ Band LF 1 Frequency` (gain at 4.0): 20 Hz → +2.43 dB plateau (low_shelf, corner 28 Hz); 43 Hz → +3.88 dB plateau (low_shelf, corner 49 Hz); 93 Hz → +3.99 dB plateau (low_shelf, corner 104 Hz); 200 Hz → +4.00 dB plateau (low_shelf, corner 224 Hz); 431 Hz → +4.00 dB plateau (low_shelf, corner 483 Hz); 928 Hz → +4.00 dB plateau (low_shelf, corner 1041 Hz); 2.00k Hz → +4.00 dB plateau (low_shelf, corner 2243 Hz)
  - q `EQ Band LF 1 Q`: 0.3 → shelf; 0.8 → bw 3.37 oct; 2.0 → bw 1.48 oct; 5.5 → bw 0.58 oct; 15.0 → bw 0.23 oct
- **EQ Band LF 2**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)
- **EQ Band LMF 1** — gain `EQ Band LMF 1 Gain`: -12.0 dB → -43.94 dB @ 316 Hz, bw 0.02 oct; -8.0 dB → -8.00 dB @ 313 Hz, bw 1.65 oct; -4.0 dB → -4.00 dB @ 313 Hz, bw 2.74 oct; 0.0 dB → flat; +4.0 dB → +4.00 dB @ 313 Hz, bw 2.74 oct; +8.0 dB → +8.00 dB @ 313 Hz, bw 1.65 oct; +12.0 dB → +12.00 dB @ 313 Hz, bw 1.47 oct
  - freq `EQ Band LMF 1 Frequency` (gain at 4.0): 20 Hz → +4.00 dB plateau (low_shelf, corner 36 Hz); 43 Hz → +3.99 dB plateau (low_shelf, corner 78 Hz); 93 Hz → +4.00 dB @ 92 Hz, bw 2.74 oct; 200 Hz → +4.00 dB @ 200 Hz, bw 2.74 oct; 431 Hz → +3.99 dB @ 430 Hz, bw 2.75 oct; 928 Hz → +4.00 dB @ 928 Hz, bw 2.75 oct; 2.00k Hz → +4.00 dB @ 2000 Hz, bw 2.73 oct
  - q `EQ Band LMF 1 Q`: 0.3 → bw 5.99 oct; 0.8 → bw 3.37 oct; 2.0 → bw 1.48 oct; 5.5 → bw 0.58 oct; 15.0 → bw 0.24 oct
- **EQ Band LMF 2**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)
- **EQ Band MF 1** — gain `EQ Band MF 1 Gain`: -12.0 dB → -31.89 dB @ 3166 Hz, bw 0.03 oct; -8.0 dB → -8.00 dB @ 3151 Hz, bw 0.86 oct; -4.0 dB → -4.00 dB @ 3151 Hz, bw 1.51 oct; 0.0 dB → flat; +4.0 dB → +4.00 dB @ 3151 Hz, bw 1.51 oct; +8.0 dB → +8.00 dB @ 3150 Hz, bw 0.86 oct; +12.0 dB → +12.00 dB @ 3151 Hz, bw 0.76 oct
  - freq `EQ Band MF 1 Frequency` (gain at 4.0): 20 Hz → +4.00 dB plateau (low_shelf, corner 27 Hz); 64 Hz → +4.00 dB @ 65 Hz, bw 1.52 oct; 206 Hz → +3.97 dB @ 207 Hz, bw 1.55 oct; 663 Hz → +3.99 dB @ 664 Hz, bw 1.52 oct; 2.13k Hz → +4.00 dB @ 2130 Hz, bw 1.52 oct; 6.85k Hz → +3.98 dB @ 6846 Hz, bw 1.50 oct; 22.00k Hz → +3.54 dB plateau (high_shelf, corner 15432 Hz)
  - q `EQ Band MF 1 Q`: 0.3 → shelf; 0.8 → bw 3.31 oct; 2.0 → bw 1.48 oct; 5.5 → bw 0.58 oct; 15.0 → bw 0.25 oct
- **EQ Band MF 2**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)

### AMEK EQ 200 1.4.1
- **HF 1** — gain `HF Gain 1`: -15.0  dB → -14.61 dB @ 1872 Hz, bw 0.95 oct; -10.0  dB → -9.32 dB @ 1871 Hz, bw 1.16 oct; -5.0  dB → -4.54 dB @ 1871 Hz, bw 1.84 oct; 0.0  dB → flat; +5.0  dB → +4.51 dB @ 1873 Hz, bw 1.88 oct; +10.0  dB → +9.24 dB @ 1873 Hz, bw 1.19 oct; +15.0  dB → +14.45 dB @ 1871 Hz, bw 1.00 oct
  - freq `HF Freq 1` (gain at 5.0): 370.0 Hz → +4.50 dB @ 383 Hz, bw 1.87 oct; 433.3 Hz → +4.51 dB @ 442 Hz, bw 1.87 oct; 660.0 Hz → +4.51 dB @ 663 Hz, bw 1.87 oct; 1.8 kHz → +4.51 dB @ 1873 Hz, bw 1.88 oct; 4.6 kHz → +4.51 dB @ 4980 Hz, bw 1.87 oct; 9.0 kHz → +4.51 dB @ 8867 Hz, bw 1.84 oct; 26.0 kHz → +3.24 dB plateau (high_shelf, corner 14771 Hz)
  - q `HF Q 1`: Shelf → shelf; 0.9 → bw 4.00 oct; 1.8 → bw 2.38 oct; 2.9 → bw 0.98 oct; 4.0 → bw 1.03 oct
- **HF 2** — gain `HF Gain 2`: -15.0  dB → -14.61 dB @ 1872 Hz, bw 0.95 oct; -10.0  dB → -9.32 dB @ 1871 Hz, bw 1.16 oct; -5.0  dB → -4.54 dB @ 1871 Hz, bw 1.84 oct; 0.0  dB → flat; +5.0  dB → +4.51 dB @ 1873 Hz, bw 1.88 oct; +10.0  dB → +9.24 dB @ 1873 Hz, bw 1.19 oct; +15.0  dB → +14.45 dB @ 1871 Hz, bw 1.00 oct
  - freq `HF Freq 2` (gain at 5.0): 370.0 Hz → +4.50 dB @ 383 Hz, bw 1.87 oct; 433.3 Hz → +4.51 dB @ 442 Hz, bw 1.87 oct; 660.0 Hz → +4.51 dB @ 663 Hz, bw 1.87 oct; 1.8 kHz → +4.51 dB @ 1873 Hz, bw 1.88 oct; 4.6 kHz → +4.51 dB @ 4980 Hz, bw 1.87 oct; 9.0 kHz → +4.51 dB @ 8867 Hz, bw 1.84 oct; 26.0 kHz → +3.24 dB plateau (high_shelf, corner 14771 Hz)
  - q `HF Q 2`: Shelf → shelf; 0.9 → bw 4.00 oct; 1.8 → bw 2.38 oct; 2.9 → bw 0.98 oct; 4.0 → bw 1.03 oct
- **HMF 1** — gain `HMF Gain 1`: -15.0  dB → -14.43 dB @ 1719 Hz, bw 0.95 oct; -10.0  dB → -9.34 dB @ 1719 Hz, bw 1.15 oct; -5.0  dB → -5.01 dB @ 1718 Hz, bw 1.67 oct; 0.0  dB → flat; +5.0  dB → +4.99 dB @ 1715 Hz, bw 1.69 oct; +10.0  dB → +9.29 dB @ 1714 Hz, bw 1.17 oct; +15.0  dB → +14.33 dB @ 1714 Hz, bw 0.98 oct
  - freq `HMF Freq 1` (gain at 5.0): 370.0 Hz → +4.99 dB @ 353 Hz, bw 1.67 oct; 433.3 Hz → +4.99 dB @ 406 Hz, bw 1.68 oct; 660.0 Hz → +4.99 dB @ 609 Hz, bw 1.68 oct; 1.8 kHz → +4.99 dB @ 1715 Hz, bw 1.69 oct; 4.6 kHz → +4.99 dB @ 4538 Hz, bw 1.68 oct; 9.0 kHz → +4.99 dB @ 8024 Hz, bw 1.67 oct; 26.0 kHz → +4.48 dB plateau (high_shelf, corner 14195 Hz)
  - q `HMF Q 1`: 0.4 → bw 4.11 oct; 1.2 → bw 3.37 oct; 2.0 → bw 1.69 oct; 3.0 → bw 0.77 oct; 4.0 → bw 0.71 oct
- **HMF 2** — gain `HMF Gain 2`: -15.0  dB → -14.43 dB @ 1719 Hz, bw 0.95 oct; -10.0  dB → -9.34 dB @ 1719 Hz, bw 1.15 oct; -5.0  dB → -5.01 dB @ 1718 Hz, bw 1.67 oct; 0.0  dB → flat; +5.0  dB → +4.99 dB @ 1715 Hz, bw 1.69 oct; +10.0  dB → +9.29 dB @ 1714 Hz, bw 1.17 oct; +15.0  dB → +14.33 dB @ 1714 Hz, bw 0.98 oct
  - freq `HMF Freq 2` (gain at 5.0): 370.0 Hz → +4.99 dB @ 353 Hz, bw 1.67 oct; 433.3 Hz → +4.99 dB @ 406 Hz, bw 1.68 oct; 660.0 Hz → +4.99 dB @ 609 Hz, bw 1.68 oct; 1.8 kHz → +4.99 dB @ 1715 Hz, bw 1.69 oct; 4.6 kHz → +4.99 dB @ 4538 Hz, bw 1.68 oct; 9.0 kHz → +4.99 dB @ 8023 Hz, bw 1.67 oct; 26.0 kHz → +4.48 dB plateau (high_shelf, corner 14195 Hz)
  - q `HMF Q 2`: 0.4 → bw 4.11 oct; 1.2 → bw 3.37 oct; 2.0 → bw 1.69 oct; 3.0 → bw 0.77 oct; 4.0 → bw 0.71 oct
- **LF 1** — gain `LF Gain 1`: -15.0  dB → -14.50 dB @ 78 Hz, bw 0.93 oct; -10.0  dB → -10.66 dB @ 78 Hz, bw 1.06 oct; -5.0  dB → -5.51 dB @ 78 Hz, bw 1.50 oct; 0.0  dB → flat; +5.0  dB → +5.51 dB @ 78 Hz, bw 1.50 oct; +10.0  dB → +10.65 dB @ 78 Hz, bw 1.07 oct; +15.0  dB → +14.48 dB @ 78 Hz, bw 0.93 oct
  - freq `LF Freq 1` (gain at 5.0): 15.0 Hz → +3.66 dB plateau (low_shelf, corner 27 Hz); 18.3 Hz → +5.34 dB plateau (low_shelf, corner 29 Hz); 29.3 Hz → +5.50 dB plateau (low_shelf, corner 44 Hz); 80.0 Hz → +5.51 dB @ 78 Hz, bw 1.50 oct; 163.3 Hz → +5.52 dB @ 156 Hz, bw 1.53 oct; 300.0 Hz → +5.51 dB @ 274 Hz, bw 1.56 oct; 780.0 Hz → +5.52 dB @ 790 Hz, bw 1.56 oct
  - q `LF Q 1`: Shelf → shelf; 0.9 → bw 3.48 oct; 1.8 → bw 1.97 oct; 2.9 → bw 0.59 oct; 4.0 → bw 0.48 oct
- **LF 2** — gain `LF Gain 2`: -15.0  dB → -14.50 dB @ 78 Hz, bw 0.93 oct; -10.0  dB → -10.66 dB @ 78 Hz, bw 1.06 oct; -5.0  dB → -5.51 dB @ 78 Hz, bw 1.50 oct; 0.0  dB → flat; +5.0  dB → +5.51 dB @ 78 Hz, bw 1.50 oct; +10.0  dB → +10.65 dB @ 78 Hz, bw 1.07 oct; +15.0  dB → +14.48 dB @ 78 Hz, bw 0.93 oct
  - freq `LF Freq 2` (gain at 5.0): 15.0 Hz → +3.65 dB plateau (low_shelf, corner 27 Hz); 18.3 Hz → +5.34 dB plateau (low_shelf, corner 29 Hz); 29.3 Hz → +5.49 dB plateau (low_shelf, corner 44 Hz); 80.0 Hz → +5.51 dB @ 78 Hz, bw 1.50 oct; 163.3 Hz → +5.52 dB @ 156 Hz, bw 1.53 oct; 300.0 Hz → +5.51 dB @ 274 Hz, bw 1.56 oct; 780.0 Hz → +5.52 dB @ 790 Hz, bw 1.56 oct
  - q `LF Q 2`: Shelf → shelf; 0.9 → bw 3.48 oct; 1.8 → bw 1.97 oct; 2.9 → bw 0.59 oct; 4.0 → bw 0.48 oct
- **LMF 1** — gain `LMF Gain 1`: -15.0  dB → -14.44 dB @ 78 Hz, bw 0.94 oct; -10.0  dB → -9.27 dB @ 78 Hz, bw 1.14 oct; -5.0  dB → -4.47 dB @ 78 Hz, bw 1.84 oct; 0.0  dB → flat; +5.0  dB → +4.46 dB @ 78 Hz, bw 1.85 oct; +10.0  dB → +9.25 dB @ 78 Hz, bw 1.15 oct; +15.0  dB → +14.40 dB @ 78 Hz, bw 0.95 oct
  - freq `LMF Freq 1` (gain at 5.0): 15.0 Hz → +2.92 dB plateau (low_shelf, corner 26 Hz); 18.3 Hz → +4.33 dB plateau (low_shelf, corner 28 Hz); 29.3 Hz → +4.44 dB plateau (low_shelf, corner 44 Hz); 80.0 Hz → +4.46 dB @ 78 Hz, bw 1.85 oct; 163.3 Hz → +4.48 dB @ 157 Hz, bw 1.88 oct; 300.0 Hz → +4.47 dB @ 276 Hz, bw 1.91 oct; 780.0 Hz → +4.48 dB @ 796 Hz, bw 1.91 oct
  - q `LMF Q 1`: 0.4 → shelf; 1.2 → bw 3.73 oct; 2.0 → bw 1.85 oct; 3.0 → bw 0.74 oct; 4.0 → bw 0.65 oct
- **LMF 2** — gain `LMF Gain 2`: -15.0  dB → -14.44 dB @ 78 Hz, bw 0.94 oct; -10.0  dB → -9.27 dB @ 78 Hz, bw 1.14 oct; -5.0  dB → -4.47 dB @ 78 Hz, bw 1.84 oct; 0.0  dB → flat; +5.0  dB → +4.46 dB @ 78 Hz, bw 1.85 oct; +10.0  dB → +9.25 dB @ 78 Hz, bw 1.15 oct; +15.0  dB → +14.40 dB @ 78 Hz, bw 0.95 oct
  - freq `LMF Freq 2` (gain at 5.0): 15.0 Hz → +2.92 dB plateau (low_shelf, corner 26 Hz); 18.3 Hz → +4.33 dB plateau (low_shelf, corner 28 Hz); 29.3 Hz → +4.44 dB plateau (low_shelf, corner 44 Hz); 80.0 Hz → +4.46 dB @ 78 Hz, bw 1.85 oct; 163.3 Hz → +4.48 dB @ 157 Hz, bw 1.88 oct; 300.0 Hz → +4.47 dB @ 276 Hz, bw 1.91 oct; 780.0 Hz → +4.48 dB @ 796 Hz, bw 1.91 oct
  - q `LMF Q 2`: 0.4 → shelf; 1.2 → bw 3.73 oct; 2.0 → bw 1.85 oct; 3.0 → bw 0.74 oct; 4.0 → bw 0.65 oct
- **MF 1** — gain `MF Gain 1`: -15.0  dB → -14.29 dB @ 556 Hz, bw 0.93 oct; -10.0  dB → -8.91 dB @ 556 Hz, bw 1.14 oct; -5.0  dB → -3.88 dB @ 556 Hz, bw 2.22 oct; 0.0  dB → flat; +5.0  dB → +3.88 dB @ 556 Hz, bw 2.23 oct; +10.0  dB → +8.90 dB @ 556 Hz, bw 1.15 oct; +15.0  dB → +14.26 dB @ 556 Hz, bw 0.94 oct
  - freq `MF Freq 1` (gain at 5.0): 120.0 Hz → +3.87 dB @ 122 Hz, bw 2.18 oct; 153.3 Hz → +3.87 dB @ 147 Hz, bw 2.20 oct; 246.7 Hz → +3.88 dB @ 223 Hz, bw 2.20 oct; 580.0 Hz → +3.88 dB @ 556 Hz, bw 2.23 oct; 1.3 kHz → +3.87 dB @ 1296 Hz, bw 2.24 oct; 2.3 kHz → +3.88 dB @ 2163 Hz, bw 2.23 oct; 7.8 kHz → +3.89 dB @ 7868 Hz, bw 2.18 oct
  - q `MF Q 1`: 0.4 → bw 5.42 oct; 1.2 → bw 4.28 oct; 2.0 → bw 2.23 oct; 3.0 → bw 0.97 oct; 4.0 → bw 0.88 oct
- **MF 2** — gain `MF Gain 2`: -15.0  dB → -14.29 dB @ 556 Hz, bw 0.93 oct; -10.0  dB → -8.91 dB @ 556 Hz, bw 1.14 oct; -5.0  dB → -3.88 dB @ 556 Hz, bw 2.22 oct; 0.0  dB → flat; +5.0  dB → +3.88 dB @ 556 Hz, bw 2.23 oct; +10.0  dB → +8.90 dB @ 556 Hz, bw 1.15 oct; +15.0  dB → +14.26 dB @ 556 Hz, bw 0.94 oct
  - freq `MF Freq 2` (gain at 5.0): 120.0 Hz → +3.87 dB @ 122 Hz, bw 2.18 oct; 153.3 Hz → +3.87 dB @ 147 Hz, bw 2.20 oct; 246.7 Hz → +3.88 dB @ 223 Hz, bw 2.20 oct; 580.0 Hz → +3.88 dB @ 556 Hz, bw 2.22 oct; 1.3 kHz → +3.87 dB @ 1296 Hz, bw 2.24 oct; 2.3 kHz → +3.88 dB @ 2163 Hz, bw 2.23 oct; 7.8 kHz → +3.89 dB @ 7870 Hz, bw 2.18 oct
  - q `MF Q 2`: 0.4 → bw 5.42 oct; 1.2 → bw 4.28 oct; 2.0 → bw 2.22 oct; 3.0 → bw 0.97 oct; 4.0 → bw 0.88 oct

### Dangerous BAX EQ Master 1.10.1
- **High Shelf 1** — gain `High Shelf Level 1`: -5.0 dB → -3.71 dB plateau (high_shelf, corner 11298 Hz); -4.5 dB → -3.38 dB plateau (high_shelf, corner 11235 Hz); -4.0 dB → -3.04 dB plateau (high_shelf, corner 11172 Hz); -3.5 dB → -2.71 dB plateau (high_shelf, corner 11120 Hz); -3.0 dB → -2.35 dB plateau (high_shelf, corner 11064 Hz); -2.5 dB → -1.99 dB plateau (high_shelf, corner 11024 Hz); -2.0 dB → -1.60 dB plateau (high_shelf, corner 10976 Hz); -1.5 dB → -1.21 dB plateau (high_shelf, corner 10945 Hz); -1.0 dB → -0.81 dB plateau (high_shelf, corner 10928 Hz); -0.5 dB → flat; 0.0 dB → flat; +0.5 dB → flat; +1.0 dB → +0.81 dB plateau (high_shelf, corner 10936 Hz); +1.5 dB → +1.21 dB plateau (high_shelf, corner 10951 Hz); +2.0 dB → +1.60 dB plateau (high_shelf, corner 10985 Hz); +2.5 dB → +1.99 dB plateau (high_shelf, corner 11023 Hz); +3.0 dB → +2.35 dB plateau (high_shelf, corner 11070 Hz); +3.5 dB → +2.71 dB plateau (high_shelf, corner 11122 Hz); +4.0 dB → +3.04 dB plateau (high_shelf, corner 11174 Hz); +4.5 dB → +3.38 dB plateau (high_shelf, corner 11235 Hz); +5.0 dB → +3.71 dB plateau (high_shelf, corner 11299 Hz)
  - freq `High Shelf Frequency 1` (gain at 5.0): 18 kHz → +3.71 dB plateau (high_shelf, corner 11299 Hz); 7.1 kHz → +5.45 dB plateau (high_shelf, corner 6186 Hz); 4.8 kHz → +5.73 dB plateau (high_shelf, corner 4384 Hz); 3.4 kHz → +5.85 dB plateau (high_shelf, corner 3186 Hz); 2.5 kHz → +5.91 dB plateau (high_shelf, corner 2530 Hz); 2.1 kHz → +5.93 dB plateau (high_shelf, corner 2069 Hz); 1.8 kHz → +5.95 dB plateau (high_shelf, corner 1805 Hz); 1.6 kHz → +5.96 dB plateau (high_shelf, corner 1555 Hz)
- **High Shelf 2** — gain `High Shelf Level 2`: -5.0 dB → -3.71 dB plateau (high_shelf, corner 11298 Hz); -4.5 dB → -3.38 dB plateau (high_shelf, corner 11235 Hz); -4.0 dB → -3.04 dB plateau (high_shelf, corner 11172 Hz); -3.5 dB → -2.71 dB plateau (high_shelf, corner 11120 Hz); -3.0 dB → -2.35 dB plateau (high_shelf, corner 11064 Hz); -2.5 dB → -1.99 dB plateau (high_shelf, corner 11024 Hz); -2.0 dB → -1.60 dB plateau (high_shelf, corner 10976 Hz); -1.5 dB → -1.21 dB plateau (high_shelf, corner 10945 Hz); -1.0 dB → -0.81 dB plateau (high_shelf, corner 10928 Hz); -0.5 dB → flat; 0.0 dB → flat; +0.5 dB → flat; +1.0 dB → +0.81 dB plateau (high_shelf, corner 10936 Hz); +1.5 dB → +1.21 dB plateau (high_shelf, corner 10951 Hz); +2.0 dB → +1.60 dB plateau (high_shelf, corner 10985 Hz); +2.5 dB → +1.99 dB plateau (high_shelf, corner 11023 Hz); +3.0 dB → +2.35 dB plateau (high_shelf, corner 11070 Hz); +3.5 dB → +2.71 dB plateau (high_shelf, corner 11122 Hz); +4.0 dB → +3.04 dB plateau (high_shelf, corner 11174 Hz); +4.5 dB → +3.38 dB plateau (high_shelf, corner 11235 Hz); +5.0 dB → +3.71 dB plateau (high_shelf, corner 11299 Hz)
  - freq `High Shelf Frequency 2` (gain at 5.0): 18 kHz → +3.71 dB plateau (high_shelf, corner 11299 Hz); 7.1 kHz → +5.45 dB plateau (high_shelf, corner 6186 Hz); 4.8 kHz → +5.73 dB plateau (high_shelf, corner 4384 Hz); 3.4 kHz → +5.85 dB plateau (high_shelf, corner 3186 Hz); 2.5 kHz → +5.91 dB plateau (high_shelf, corner 2530 Hz); 2.1 kHz → +5.93 dB plateau (high_shelf, corner 2069 Hz); 1.8 kHz → +5.95 dB plateau (high_shelf, corner 1805 Hz); 1.6 kHz → +5.96 dB plateau (high_shelf, corner 1555 Hz)
- **Low Shelf 1** — gain `Low Shelf Level 1`: -5.0 dB → -6.06 dB plateau (low_shelf, corner 161 Hz); -4.5 dB → -5.47 dB plateau (low_shelf, corner 155 Hz); -4.0 dB → -4.90 dB plateau (low_shelf, corner 150 Hz); -3.5 dB → -4.34 dB plateau (low_shelf, corner 146 Hz); -3.0 dB → -3.74 dB plateau (low_shelf, corner 142 Hz); -2.5 dB → -3.16 dB plateau (low_shelf, corner 139 Hz); -2.0 dB → -2.53 dB plateau (low_shelf, corner 136 Hz); -1.5 dB → -1.92 dB plateau (low_shelf, corner 134 Hz); -1.0 dB → -1.28 dB plateau (low_shelf, corner 133 Hz); -0.5 dB → -0.65 dB plateau (low_shelf, corner 132 Hz); 0.0 dB → flat; +0.5 dB → +0.65 dB plateau (low_shelf, corner 132 Hz); +1.0 dB → +1.28 dB plateau (low_shelf, corner 133 Hz); +1.5 dB → +1.92 dB plateau (low_shelf, corner 134 Hz); +2.0 dB → +2.54 dB plateau (low_shelf, corner 136 Hz); +2.5 dB → +3.16 dB plateau (low_shelf, corner 139 Hz); +3.0 dB → +3.75 dB plateau (low_shelf, corner 142 Hz); +3.5 dB → +4.34 dB plateau (low_shelf, corner 146 Hz); +4.0 dB → +4.90 dB plateau (low_shelf, corner 150 Hz); +4.5 dB → +5.47 dB plateau (low_shelf, corner 155 Hz); +5.0 dB → +6.06 dB plateau (low_shelf, corner 161 Hz)
  - freq `Low Shelf Frequency 1` (gain at 5.0): 74 Hz → +6.06 dB plateau (low_shelf, corner 161 Hz); 84 Hz → +6.08 dB plateau (low_shelf, corner 182 Hz); 98 Hz → +6.10 dB plateau (low_shelf, corner 211 Hz); 116 Hz → +6.12 dB plateau (low_shelf, corner 249 Hz); 131 Hz → +6.13 dB plateau (low_shelf, corner 279 Hz); 166 Hz → +6.14 dB plateau (low_shelf, corner 351 Hz); 230 Hz → +6.15 dB plateau (low_shelf, corner 487 Hz); 361 Hz → +6.16 dB plateau (low_shelf, corner 765 Hz)
- **Low Shelf 2** — gain `Low Shelf Level 2`: -5.0 dB → -6.06 dB plateau (low_shelf, corner 161 Hz); -4.5 dB → -5.47 dB plateau (low_shelf, corner 155 Hz); -4.0 dB → -4.90 dB plateau (low_shelf, corner 150 Hz); -3.5 dB → -4.34 dB plateau (low_shelf, corner 146 Hz); -3.0 dB → -3.74 dB plateau (low_shelf, corner 142 Hz); -2.5 dB → -3.16 dB plateau (low_shelf, corner 139 Hz); -2.0 dB → -2.53 dB plateau (low_shelf, corner 136 Hz); -1.5 dB → -1.92 dB plateau (low_shelf, corner 134 Hz); -1.0 dB → -1.28 dB plateau (low_shelf, corner 133 Hz); -0.5 dB → -0.65 dB plateau (low_shelf, corner 132 Hz); 0.0 dB → flat; +0.5 dB → +0.65 dB plateau (low_shelf, corner 132 Hz); +1.0 dB → +1.28 dB plateau (low_shelf, corner 133 Hz); +1.5 dB → +1.92 dB plateau (low_shelf, corner 134 Hz); +2.0 dB → +2.54 dB plateau (low_shelf, corner 136 Hz); +2.5 dB → +3.16 dB plateau (low_shelf, corner 139 Hz); +3.0 dB → +3.75 dB plateau (low_shelf, corner 142 Hz); +3.5 dB → +4.34 dB plateau (low_shelf, corner 146 Hz); +4.0 dB → +4.90 dB plateau (low_shelf, corner 150 Hz); +4.5 dB → +5.47 dB plateau (low_shelf, corner 155 Hz); +5.0 dB → +6.06 dB plateau (low_shelf, corner 161 Hz)
  - freq `Low Shelf Frequency 2` (gain at 5.0): 74 Hz → +6.06 dB plateau (low_shelf, corner 161 Hz); 84 Hz → +6.08 dB plateau (low_shelf, corner 182 Hz); 98 Hz → +6.10 dB plateau (low_shelf, corner 211 Hz); 116 Hz → +6.12 dB plateau (low_shelf, corner 249 Hz); 131 Hz → +6.13 dB plateau (low_shelf, corner 279 Hz); 166 Hz → +6.14 dB plateau (low_shelf, corner 351 Hz); 230 Hz → +6.15 dB plateau (low_shelf, corner 487 Hz); 361 Hz → +6.16 dB plateau (low_shelf, corner 765 Hz)

### Lindell PEX-500 1.2.0
- **Hi** — gain `Hi attenuation`: 0.0 → flat; 1.7 → -1.00 dB plateau (high_shelf, corner 13986 Hz); 3.3 → -2.15 dB plateau (high_shelf, corner 13974 Hz); 5.0 → -3.50 dB plateau (high_shelf, corner 13945 Hz); 6.7 → -5.13 dB plateau (high_shelf, corner 13890 Hz); 8.3 → -7.20 dB plateau (high_shelf, corner 13792 Hz); 10.0 → -10.02 dB plateau (high_shelf, corner 13596 Hz)
  - freq `Hi frequency` (gain at 6.7): 3kHz → -5.13 dB plateau (high_shelf, corner 13890 Hz); 4kHz → -5.13 dB plateau (high_shelf, corner 13889 Hz); 5kHz → -5.13 dB plateau (high_shelf, corner 13890 Hz); 6kHz → -5.13 dB plateau (high_shelf, corner 13890 Hz); 8kHz → -5.13 dB plateau (high_shelf, corner 13890 Hz); 10kHz → -5.13 dB plateau (high_shelf, corner 13890 Hz); 16kHz → -5.13 dB plateau (high_shelf, corner 13889 Hz)
  - q `Hi bandwidth`: 0.0 → shelf; 2.5 → shelf; 5.0 → shelf; 7.5 → shelf; 10.0 → shelf
- **Hi mid**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)
- **Hi side**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)
- **Low** — gain `Low boost`: 0.0 → flat; 1.7 → +3.25 dB plateau (low_shelf, corner 1000 Hz); 3.3 → +5.36 dB plateau (low_shelf, corner 597 Hz); 5.0 → +6.91 dB plateau (low_shelf, corner 463 Hz); 6.7 → +10.48 dB plateau (low_shelf, corner 309 Hz); 8.3 → +13.59 dB plateau (low_shelf, corner 244 Hz); 10.0 → +14.97 dB plateau (low_shelf, corner 224 Hz)
  - freq `Low frequency` (gain at 6.7): 30Hz → +10.40 dB plateau (low_shelf, corner 211 Hz); 60Hz → +10.48 dB plateau (low_shelf, corner 309 Hz); 100Hz → +10.55 dB plateau (low_shelf, corner 663 Hz)
- **Low mid**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)
- **Low side**: flat at every gain position (a band not enabled, or a channel-2 / side control of a linked unit)


What this says:
- **Digital EQs tell the truth, with a character**: bx_digital V3's frequency labels match the measured centre within 0.5 %
  (400 → 400, 1.52k → 1528, 5.78k → 5752, 11.28k → 11253) and its gains within 0.1 dB up to ±8, but its **Q is proportional**:
  bandwidth 1.08 oct at ±4 dB, 0.59 at ±8, 0.52 at +12 — and at −12 the notch reads −15.3 dB in a 0.14-octave slot (the cut
  deepens beyond its label as it narrows). bx_2098 as its name promises.
- **AMEK EQ 200's centres sit 4–10 % below the label** (370 → 353, 433 → 407, 660 → 609, 1.8k → 1715, 4.6k → 4538, 9.0k → 8024)
  with gains within 0.7 dB (±15 → ±14.4) and proportional Q (1.7 oct at 5 dB → 0.96 at 15); its Q label 0.4–4.0 maps to 4.1–0.75 oct.
- **The Pultec (PEX-500) is the case the roadmap predicted**: "Low boost 0–10" is not dB (1.7 → 3.2 dB, 5.0 → 6.9, 10 → 15.0
  plateau), and its "30 / 60 / 100 Hz" frequencies put the half-gain corner at 211 / 309 / 663 Hz — the label is the shelf's
  turnover far below, which is the famous Pultec shape. "Hi attenuation" and the mid/side bands are flat (M/S off; attenuation needs
  its own baseline — a lead).
- **Dangerous BAX's shelf labels read 1.2×** (−5.0 → −6.05, +1.5 → +1.92, 0.5 → 0.65 dB).
- **Bands that read flat** on bx_digital (every "2" band) and the PEX "mid / side" bands are the second channel of a linked unit or
  a band not enabled: the compressor's engage search (a two-state "on" near the band's name) is the next step for EQs.

## Draft schema (for Sean)

```json
"eq": {
  "bands": [ { "name": "HMF", "gain_control": "HMF Gain 1", "freq_control": "HMF Freq 1", "q_control": "HMF Q 1",
             "shape": "peak", "proportional_q": true,
             "gain_map":  [ { "display": "+5.0 dB", "norm": 0.67, "gain_db": 4.97 }, "…" ],
             "freq_map":  [ { "display": "1.8 kHz", "norm": 0.5, "centre_hz": 1715 }, "…" ],
             "q_map":     [ { "display": "2.0", "norm": 0.5, "bandwidth_oct": 1.69 }, "…" ] },
           { "name": "Low", "shape": "low_shelf", "gain_map": [ { "display": "6.7", "norm": 0.67, "gain_db": 10.48 } ],
             "freq_map": [ { "display": "60Hz", "norm": 0.5, "corner_hz": 309 } ] } ],
  "method": "121-tone multitone, deviation against the unit's own baseline; centre to 3 %, bandwidth to 0.1 oct"
}
```
Acceptance as the roadmap put it: ask for "+2 dB at 3 kHz", write the mapped norms, and re-measure within 0.5 dB and the stated
frequency tolerance (3 %).

## Questions for Sean

1. The frequency label's meaning differs by design (centre for a peak, half-gain corner for a shelf, turnover for a Pultec):
   does the server want one field per shape, or one `centre_hz` with `shape` deciding?
2. Proportional Q (bx_digital, AMEK): the bandwidth depends on the gain. Map Q at the gain the server will write, or carry a
   small gain × Q grid?
3. Bands with an enable switch: run the engage search per band (cost: one response per candidate switch), or treat a flat band
   as "needs the switch" in the record and let the server leave it?
4. The grid: 1/12 octave is enough for peaks down to ~0.3 octave; narrow notches (Q 15: bx_digital's "broad" result is a notch
   between tones) need a denser grid or a chirp. Agree 1/24 octave (241 tones) for Q positions only?
