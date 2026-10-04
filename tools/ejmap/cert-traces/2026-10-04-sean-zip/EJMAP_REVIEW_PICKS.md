# EJ Map review picks — Sean's 3 Oct run, after the 4 Oct rules (generated 2026-10-04)

11 products are still `needs_review` with several threshold candidates and no rule to decide them.
For each, the candidates are listed with their sweep verdict and 2 dB curve (the input level, dBFS RMS, at which that
position gives 2 dB of gain reduction; `not_reached` = the sweep never got 2 dB there; `below_range` = already past 2 dB at
the quietest level). A pick means: that control is the amount; every other candidate stays at its instantiate value.

**To pick:** add one entry per product to `review_picks.json` (template beside this file), e.g.

```json
[{"product": "Shadow Hills Mastering Compressor", "candidate": "Optical Threshold 1", "by": "KD", "date": "2026-10-05", "note": "optical stage; channel 1 drives both"}]
```

Only a candidate whose verdict is `certified` can be picked (an entry naming any other picks nothing and the log says why).
The file goes into Sean's `~/Library/ejmap/cert/` before the follow-up runs; nothing is picked without an entry.

## kHs Dynamics (2.1.0) — 2 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** none — no candidate certified; nothing can be picked from this run

- **[2] Low Threshold** — flat. the LOW band / low-level threshold; prints −50.0 dB .. 0.0 dB dB; instantiates at '−30.0\u202fdB'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[4] High Threshold** — flat. the HIGH band / high-level threshold; prints −50.0 dB .. 0.0 dB dB; instantiates at '−20.0\u202fdB'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading

## MaxxVolume (m) (15.0.70) — 2 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** Low Level Thresh, High Level Thresh

- **[0] Low Level Thresh** — certified. the low-level stage (leveller/upward) threshold; prints -96.0 .. 0.0 dB; instantiates at '-96.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.07:not_reached 0.13:not_reached 0.20:not_reached 0.27:not_reached 0.33:-60.6 0.36:-58.0 0.38:-55.5 0.39:-54.2 0.40:-52.3 0.42:-50.0 0.44:-47.7 0.47:-45.4 0.49:-43.1 0.51:-40.2 0.53:-38.1 0.56:-36.1 0.58:-34.1 0.60:-32.0 0.62:-29.9 0.64:-27.3 0.67:-25.4 0.70:-22.5 0.73:-19.5 0.76:-17.5 0.78:-15.5 0.80:-13.2 0.82:-10.9 0.84:-8.4 0.87:-5.8 0.93:not_reached 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[4] High Level Thresh** — certified. the high-level stage (compressor/downward) threshold; prints -48.0 .. 0.0 dB; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:-62.1 0.03:-59.6 0.07:-57.1 0.10:-54.6 0.13:-51.7 0.17:-49.3 0.20:-47.1 0.23:-44.8 0.27:-42.7 0.30:-40.0 0.33:-37.9 0.37:-35.9 0.40:-33.9 0.43:-31.9 0.47:-29.2 0.50:-27.3 0.53:-25.5 0.57:-23.5 0.60:-21.5 0.63:-19.5 0.67:-17.5 0.70:-15.3 0.73:-12.9 0.77:-10.6 0.80:-7.7 0.87:not_reached 0.93:not_reached 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map

## MaxxVolume (s) (15.0.70) — 2 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** Low Level Thresh, High Level Thresh

- **[0] Low Level Thresh** — certified. the low-level stage (leveller/upward) threshold; prints -96.0 .. 0.0 dB; instantiates at '-96.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.07:not_reached 0.13:not_reached 0.20:not_reached 0.27:not_reached 0.33:-60.6 0.36:-58.0 0.38:-55.5 0.39:-54.2 0.40:-52.3 0.42:-50.0 0.44:-47.7 0.47:-45.4 0.49:-43.1 0.51:-40.2 0.53:-38.1 0.56:-36.1 0.58:-34.1 0.60:-32.0 0.62:-29.9 0.64:-27.3 0.67:-25.4 0.70:-22.5 0.73:-19.5 0.76:-17.5 0.78:-15.5 0.80:-13.2 0.82:-10.9 0.84:-8.4 0.87:-5.8 0.93:not_reached 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[4] High Level Thresh** — certified. the high-level stage (compressor/downward) threshold; prints -48.0 .. 0.0 dB; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:-62.1 0.03:-59.6 0.07:-57.1 0.10:-54.6 0.13:-51.7 0.17:-49.3 0.20:-47.1 0.23:-44.8 0.27:-42.7 0.30:-40.0 0.33:-37.9 0.37:-35.9 0.40:-33.9 0.43:-31.9 0.47:-29.2 0.50:-27.3 0.53:-25.5 0.57:-23.5 0.60:-21.5 0.63:-19.5 0.67:-17.5 0.70:-15.3 0.73:-12.9 0.77:-10.6 0.80:-7.7 0.87:not_reached 0.93:not_reached 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map

## dbx-160 (s) (15.0.70) — 2 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** none — no candidate certified; nothing can be picked from this run

- **[2] Threshold L/M** — flat. a threshold; channel 1/left; prints 0.010 .. 3.000; instantiates at '1.000'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input plus a constant 0.31 dB within 0.01 dB at every reading
- **[3] Threshold R/S** — flat. a threshold; channel 2/right; prints 0.010 .. 3.000; instantiates at '1.000'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input plus a constant 0.31 dB within 0.01 dB at every reading

## OTT (1.3.7) — 3 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** none — no candidate certified; nothing can be picked from this run

- **[5] Thresh L** — unreadable. a threshold; channel 1/left; prints 0 .. 200 %; instantiates at '100'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: no two end positions readable at every level
- **[6] Thresh M** — nonmonotonic. a threshold; prints 0 .. 200 %; instantiates at '100'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:-59.8 0.07:-59.1 0.13:not_reached 0.20:not_reached 0.27:not_reached 0.33:not_reached 0.40:not_reached 0.47:not_reached 0.53:not_reached 0.60:not_reached 0.67:not_reached 0.73:below_range 0.80:below_range 0.87:below_range 0.93:below_range 1.00:below_range
  - sweep note: reduction falls by more than 0.5 dB walking from the softer end (position 12 at -60.00)
- **[7] Thresh H** — unreadable. a threshold; prints 0 .. 200 %; instantiates at '100'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: no two end positions readable at every level

## VBC Rack (1.3.5) — 4 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** MU Threshold Left, MU Threshold Right

- **[2] Red Threshold** — flat. a threshold; prints 0.0dB .. -50.0dB dB; instantiates at '0.0dB'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: no two positions differ by more than 1 dB at any test level
- **[16] Grey Threshold** — flat. a threshold; prints 0.0dB .. -50.0dB dB; instantiates at '0.0dB'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: no two positions differ by more than 1 dB at any test level
- **[22] MU Threshold Left** — certified. a threshold; channel 1/left; prints 12.0 .. -48.0; instantiates at '12.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.07:not_reached 0.13:not_reached 0.20:not_reached 0.27:-4.5 0.33:-6.3 0.40:-8.3 0.47:-10.7 0.53:-13.5 0.57:-15.1 0.60:-16.7 0.63:-18.4 0.67:-20.2 0.70:-22.0 0.73:-23.8 0.77:-25.6 0.80:-27.4 0.83:-29.2 0.87:-31.6 0.90:-33.5 0.93:-35.5 0.97:-37.4 1.00:-39.2
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[24] MU Threshold Right** — certified. a threshold; channel 2/right; prints 12.0 .. -48.0; instantiates at '12.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.07:not_reached 0.13:not_reached 0.20:not_reached 0.27:-4.5 0.33:-6.3 0.40:-8.3 0.47:-10.7 0.53:-13.5 0.57:-15.1 0.60:-16.7 0.63:-18.4 0.67:-20.2 0.70:-22.0 0.73:-23.8 0.77:-25.6 0.80:-27.4 0.83:-29.2 0.87:-31.6 0.90:-33.5 0.93:-35.5 0.97:-37.4 1.00:-39.2
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map

## DynOne3 (3.12.3) — 15 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** none — no candidate certified; nothing can be picked from this run

- **[0] C HMF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[3] LR MF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[20] S HF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[21] C LF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[26] C MF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[30] S HMF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[47] S LF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[50] S MF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[55] C LMF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[56] LR HMF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[78] S LMF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[81] LR HF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[103] C HF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[107] LR LMF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading
- **[111] LR LF Threshold** — flat. a threshold; prints -60.0 .. 0.0; instantiates at '-40.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: passthrough: output equals input within 0.01 dB at every reading

## Maag MAGNUM-K (1.6.1) — 6 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** Threshold 1, K Comp Threshold 1, Limiter Threshold 1, Threshold 2, K Comp Threshold 2, Limiter Threshold 2

- **[6] Threshold 1** — certified. a threshold; channel 1/left; prints 0.0 .. 10.0; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.05:not_reached 0.10:not_reached 0.15:not_reached 0.20:not_reached 0.25:not_reached 0.30:not_reached 0.35:not_reached 0.40:not_reached 0.45:not_reached 0.50:not_reached 0.55:not_reached 0.60:not_reached 0.65:not_reached 0.70:-4.3 0.75:-6.3 0.80:-8.0 0.85:-9.5 0.90:-10.4 0.95:-10.6 1.00:-10.6
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[10] K Comp Threshold 1** — certified. a threshold; channel 1/left; prints 0.0 .. 10.0; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.05:not_reached 0.10:not_reached 0.15:not_reached 0.20:not_reached 0.25:not_reached 0.30:not_reached 0.35:not_reached 0.40:not_reached 0.45:not_reached 0.50:not_reached 0.55:not_reached 0.60:not_reached 0.65:not_reached 0.70:not_reached 0.75:not_reached 0.80:not_reached 0.85:not_reached 0.90:not_reached 0.95:not_reached 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[16] Limiter Threshold 1** — certified. a limiter stage threshold; channel 1/left; prints -15 dBFS .. -3 dBFS dBFS; instantiates at '-3 dBFS'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:-8.6 0.33:-5.8 0.67:not_reached 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[28] Threshold 2** — certified. a threshold; channel 2/right; prints 0.0 .. 10.0; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.05:not_reached 0.10:not_reached 0.15:not_reached 0.20:not_reached 0.25:not_reached 0.30:not_reached 0.35:not_reached 0.40:not_reached 0.45:not_reached 0.50:not_reached 0.55:not_reached 0.60:not_reached 0.65:not_reached 0.70:-4.3 0.75:-6.3 0.80:-8.0 0.85:-9.5 0.90:-10.4 0.95:-10.6 1.00:-10.6
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[32] K Comp Threshold 2** — certified. a threshold; channel 2/right; prints 0.0 .. 10.0; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.05:not_reached 0.10:not_reached 0.15:not_reached 0.20:not_reached 0.25:not_reached 0.30:not_reached 0.35:not_reached 0.40:not_reached 0.45:not_reached 0.50:not_reached 0.55:not_reached 0.60:not_reached 0.65:not_reached 0.70:not_reached 0.75:not_reached 0.80:not_reached 0.85:not_reached 0.90:not_reached 0.95:not_reached 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[38] Limiter Threshold 2** — certified. a limiter stage threshold; channel 2/right; prints -15 dBFS .. -3 dBFS dBFS; instantiates at '-3 dBFS'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:-8.6 0.33:-5.8 0.67:not_reached 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map

## Shadow Hills Class A Mastering Comp (1.4.1) — 4 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** Optical Threshold 1, Discrete Threshold 1, Optical Threshold 2, Discrete Threshold 2

- **[17] Optical Threshold 1** — certified. the optical (opto) stage's threshold; channel 1/left; prints 1 .. 24; instantiates at '1'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.04:not_reached 0.09:not_reached 0.13:not_reached 0.17:not_reached 0.22:-3.1 0.26:-5.4 0.30:-7.5 0.35:-9.6 0.39:-11.7 0.43:-13.7 0.48:-14.7 0.52:-15.6 0.57:-16.6 0.61:-17.6 0.65:-18.5 0.70:-19.5 0.74:-20.4 0.78:-21.4 0.83:-22.3 0.87:-23.3 0.91:-24.3 0.96:-25.3 1.00:-26.2
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[20] Discrete Threshold 1** — certified. the discrete (VCA) stage's threshold; channel 1/left; prints 1 .. 24; instantiates at '1'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.04:not_reached 0.09:not_reached 0.13:not_reached 0.17:not_reached 0.22:not_reached 0.26:-4.5 0.30:-7.5 0.35:-10.5 0.39:-13.4 0.43:-16.3 0.48:-19.1 0.52:-22.0 0.57:-24.9 0.61:-27.5 0.65:-30.0 0.70:-32.9 0.72:not_reached 0.74:-36.2 0.76:not_reached 0.78:-39.6 0.80:not_reached 0.83:-43.4 0.85:not_reached 0.87:-46.6 0.89:not_reached 0.91:-50.0 0.96:-53.0 0.98:not_reached 1.00:-56.5
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[29] Optical Threshold 2** — certified. the optical (opto) stage's threshold; channel 2/right; prints 1 .. 24; instantiates at '1'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.04:not_reached 0.09:not_reached 0.13:not_reached 0.17:not_reached 0.22:not_reached 0.26:not_reached 0.30:not_reached 0.35:-4.7 0.39:-6.8 0.43:-8.7 0.48:-9.7 0.52:-10.6 0.57:-11.6 0.61:-12.5 0.65:-13.4 0.70:-14.3 0.74:-15.3 0.78:-16.1 0.83:-17.1 0.87:-17.9 0.91:-18.8 0.96:-19.6 1.00:-20.4
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[32] Discrete Threshold 2** — certified. the discrete (VCA) stage's threshold; channel 2/right; prints 1 .. 24; instantiates at '1'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.04:not_reached 0.09:not_reached 0.13:not_reached 0.17:not_reached 0.22:not_reached 0.26:not_reached 0.30:not_reached 0.35:not_reached 0.39:-4.4 0.43:-7.3 0.48:-10.0 0.52:-12.9 0.57:-15.9 0.61:-18.4 0.65:-20.9 0.70:-23.8 0.72:not_reached 0.74:-27.1 0.76:not_reached 0.78:-30.5 0.80:not_reached 0.83:-34.3 0.85:not_reached 0.87:-37.5 0.89:not_reached 0.91:-40.9 0.96:-43.6 0.98:not_reached 1.00:-47.5
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map

## Shadow Hills Mastering Compressor (1.5.1) — 4 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** Optical Threshold 1, Discrete Threshold 1, Optical Threshold 2, Discrete Threshold 2

- **[2] Optical Threshold 1** — certified. the optical (opto) stage's threshold; channel 1/left; prints 1 .. 24; instantiates at '1'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.04:not_reached 0.09:not_reached 0.13:not_reached 0.17:-4.8 0.22:-7.2 0.26:-9.5 0.30:-11.6 0.35:-13.7 0.39:-15.8 0.43:-17.8 0.48:-18.8 0.52:-19.8 0.57:-20.7 0.61:-21.7 0.65:-22.6 0.70:-23.6 0.74:-24.5 0.78:-25.5 0.83:-26.4 0.87:-27.4 0.91:-28.4 0.96:-29.4 1.00:-30.3
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[5] Discrete Threshold 1** — certified. the discrete (VCA) stage's threshold; channel 1/left; prints 1 .. 24; instantiates at '1'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.04:not_reached 0.09:not_reached 0.13:-3.4 0.17:-6.2 0.22:-8.9 0.26:-11.6 0.30:-14.3 0.35:-17.0 0.39:-19.7 0.43:-22.3 0.48:-25.0 0.52:-27.6 0.57:-30.3 0.61:-32.9 0.65:-35.6 0.70:-38.3 0.74:-41.0 0.78:-43.7 0.83:-46.5 0.87:-49.2 0.91:-52.0 0.96:-54.8 1.00:-57.7
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[14] Optical Threshold 2** — certified. the optical (opto) stage's threshold; channel 2/right; prints 1 .. 24; instantiates at '1'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.04:not_reached 0.09:not_reached 0.13:not_reached 0.17:not_reached 0.22:not_reached 0.26:-3.5 0.30:-5.6 0.35:-7.7 0.39:-9.7 0.43:-11.7 0.48:-12.7 0.52:-13.6 0.57:-14.6 0.61:-15.5 0.65:-16.4 0.70:-17.3 0.74:-18.2 0.78:-19.1 0.83:-19.9 0.87:-20.8 0.91:-21.6 0.96:-22.4 1.00:-23.2
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[17] Discrete Threshold 2** — certified. the discrete (VCA) stage's threshold; channel 2/right; prints 1 .. 24; instantiates at '1'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:not_reached 0.04:not_reached 0.09:not_reached 0.13:not_reached 0.17:not_reached 0.22:not_reached 0.26:-4.5 0.30:-7.1 0.35:-9.8 0.39:-12.5 0.43:-15.1 0.48:-17.8 0.52:-20.4 0.57:-23.1 0.61:-25.8 0.65:-28.4 0.70:-31.1 0.74:-33.8 0.78:-36.5 0.83:-39.3 0.87:-42.0 0.91:-44.9 0.96:-47.7 1.00:-50.5
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map

## Auto-Tune Vocal Compressor (1.5.0) — 4 threshold candidates (a channel strip or multiband): no rule decides it, nobody picks

**Pickable (certified):** Mod Comp 1 Thresh dB

- **[18] Mod Comp 1 Thresh dB** — certified. a threshold; channel 1/left; prints -60.0 .. 0.0; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 0.00:-60.3 0.03:-58.3 0.07:-56.2 0.10:-54.3 0.13:-52.3 0.17:-50.3 0.20:-48.3 0.23:-46.3 0.27:-44.2 0.30:-42.3 0.33:-40.3 0.37:-38.3 0.40:-36.3 0.43:-34.3 0.47:-32.3 0.50:-30.3 0.53:-28.3 0.57:-26.3 0.60:-24.3 0.63:-22.3 0.67:-20.3 0.70:-18.3 0.73:-16.3 0.77:-14.3 0.80:-12.3 0.83:-10.3 0.87:-8.3 0.90:-6.3 0.93:-4.3 1.00:not_reached
  - sweep note: ratio unknown (no ratio control): no dB-equivalent map
- **[30] Mod Comp 2 Thresh dB** — flat. a threshold; channel 2/right; prints -60.0 .. 0.0; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: no two positions differ by more than 1 dB at any test level
- **[46] Opt B Comp 1 Threshold dB** — flat. a threshold; channel 2/right; prints -60.0 .. 0.0; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: no two positions differ by more than 1 dB at any test level
- **[52] Opt B Comp 2 Threshold dB** — flat. a threshold; channel 2/right; prints -60.0 .. 0.0; instantiates at '0.0'
  - 2 dB curve (norm : input dBFS RMS at 2 dB GR): 
  - sweep note: no two positions differ by more than 1 dB at any test level
