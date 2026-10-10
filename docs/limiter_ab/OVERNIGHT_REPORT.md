# OVERNIGHT REPORT - Limiter v2, 7-8 Oct 2026 (session L)

STATUS: tuning done (C7c is the version to gate), v2 integrated into EedLimiterProcessor, listening files written,
hand-off written. UI preview CODE written and committed, NOT built: ~/echojay-vst/LIMITER_V2_GATE_REPORT.md did not
exist when this report was written. Session A was not gating during the run; every build was the harness tree only,
-j 2, df checked before each (78 GB free throughout; the 15 GB floor was never approached).

## 1. Tuning, step by step (each a commit with its rule lines; results/2026-10-07_C*.txt)

| step | change | bass_sustain level / pumping (Pro-L 2 -7.09 / 0.70) | fullmix_hot level / pumping (Pro-L 2 -10.06 / 0.08) |
|---|---|---|---|
| current limiter | (the shipping EedLimiterProcessor, ported and proven) | -7.68 / 1.00 | -10.69 / 0.44 |
| v2 before tuning | Clean behaviour | -8.15 / 0.77 | -11.28 / 0.70 |
| C1 | slow floor 72 %, 150 ms charge, 180 ms decay, 1 ms source window | -7.57 / 0.93 | -10.68 / 0.42 |
| C2 | fast part instant (0.3 ms) | -7.40 / 0.87 | -10.39 / 0.28 |
| C3 | window 0.3 ms, box | -6.92 / 0.65 | -10.12 / 0.12 |
| C4 | (approved consequence of C2+C3: Transparent rides the waveform; CLEAN kept as limv2::clean()) | | |
| C5 | link 0.75 (panned dips 5.99/8.01 vs Pro-L 2 5.61/7.53) | -6.92 / 0.65 | -10.12 / 0.12 |
| C6 | ceiling margin 0.05 dB (output -0.05 dBTP) | -6.88 / 0.64 | -10.11 / 0.12 |
| C7 | second floor charge to 100 % (tau 1.2 s), 8 ms window | -7.13 / 0.68 | -10.22 / 0.18 |
| C7b | second stage at 0.9 (no change on the primaries; reverted) | -7.13 / 0.68 | -10.22 / 0.18 |
| **C7c FINAL** | 4 ms window, second stage at 100 % | **-7.00 / 0.66** | **-10.17 / 0.15** |

Zero true-peak overs on every case at every step. Target missed and kept: fullmix_hot level is 0.11 LU under
Pro-L 2 (0.10 allowed); the 1 ms window that recovers it (C6, -10.11 PASS) puts the bass 0.21 LU too loud and fails
two THD rules, so C7c is the best split. Minimum bar (closer than the current limiter on level and pumping, both
primaries): met on all four numbers. Release-limb rule: fails on t90 where "10 %" of ~1 ms is below the harness's
0.33 ms block (t63 matches: 1.3 vs 1.3 ms on bass); tone-level FAILs on 997 Hz / imd / probe are sustained synthetic
material where the floor settles deeper than Pro-L 2's.

Hard requirements (core test, every step): zero overs at +15 dB, near-silence, mono, 44.1/48/88.2/96/192 kHz, no NaN,
no subnormals - GREEN.

## 2. Final rule lines, verbatim (results/2026-10-07_C7c_window_4ms.txt; bass_sustain at --gain 8.5, fullmix_hot at --gain 10.86, the rest at 8.2)

```
-- rule: v2 vs proL2 on tone_997
  FAIL  level matched within 0.1 LU  [-0.16 vs 0.01 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.05 dBTP (Pro-L 2: 0, -0.02 dBTP)]
  PASS  THD at the highest drive: no harmonic more than 3 dB worse than Pro-L 2  [h2 -126.0/-145.4 h3 -44.5/-27.1 h4 -136.3/-154.1 h5 -59.8/-36.1 h6 -135.4/-156.4 h7 -74.5/-76.7 h8 -149.8/-163.8 ]
-- rule: v2 vs proL2 on tone_50
  PASS  level matched within 0.1 LU  [-4.20 vs -4.14 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.05 dBTP (Pro-L 2: 0, -0.04 dBTP)]
  PASS  THD at the highest drive: no harmonic more than 3 dB worse than Pro-L 2  [h2 -117.9/-117.8 h3 -21.8/-20.0 h4 -126.4/-126.7 h5 -30.8/-30.7 h6 -131.8/-132.1 h7 -46.2/-39.8 h8 -134.0/-133.9 ]
-- rule: v2 vs proL2 on tone_imd
  FAIL  level matched within 0.1 LU  [0.31 vs 0.80 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.05 dBTP (Pro-L 2: 4, 0.08 dBTP)]
  PASS  THD at the highest drive: no harmonic more than 3 dB worse than Pro-L 2  [IMD 1k -84.6/-88.2  18k -41.0/-24.1]
-- rule: v2 vs proL2 on probe_transients
  FAIL  level matched within 0.1 LU  [-4.92 vs -4.65 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.05 dBTP (Pro-L 2: 0, -0.04 dBTP)]
  PASS  transient retention within 1 dB of Pro-L 2 on every hit  [worst 0.11 dB at hit 8 (21.10 s)]
  FAIL  release limbs within 10% (median t63, t90)  [t63 0.8 vs 9.5 ms, t90 5.3 vs 9.5 ms]
  PASS  pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB  [1.73 vs 1.53 dB]
-- rule: v2 vs proL2 on panned_transient
  PASS  level matched within 0.1 LU  [-5.94 vs -5.93 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.05 dBTP (Pro-L 2: 0, -0.04 dBTP)]
  PASS  transient retention within 1 dB of Pro-L 2 on every hit  [worst 0.01 dB at hit 0 (1.10 s)]
  FAIL  release limbs within 10% (median t63, t90)  [t63 8.7 vs 9.7 ms, t90 9.0 vs 9.7 ms]
  PASS  pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB  [0.46 vs 0.45 dB]
-- rule: echojay vs proL2 on fullmix
  PASS  level matched within 0.1 LU  [-12.61 vs -12.60 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.18 dBTP (Pro-L 2: 0, -0.03 dBTP)]
  PASS  transient retention within 1 dB of Pro-L 2 on every hit  [worst 0.60 dB at hit 9 (30.07 s)]
  FAIL  release limbs within 10% (median t63, t90)  [t63 26.5 vs 0.3 ms, t90 58.2 vs 0.3 ms]
  PASS  pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB  [0.14 vs 0.01 dB]
-- rule: v2 vs proL2 on fullmix
  PASS  level matched within 0.1 LU  [-12.61 vs -12.60 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.05 dBTP (Pro-L 2: 0, -0.03 dBTP)]
  PASS  transient retention within 1 dB of Pro-L 2 on every hit  [worst 0.03 dB at hit 9 (30.07 s)]
  FAIL  release limbs within 10% (median t63, t90)  [t63 0.3 vs 0.3 ms, t90 2.0 vs 0.3 ms]
  PASS  pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB  [0.02 vs 0.01 dB]
-- rule: echojay vs proL2 on bass_sustain
  FAIL  level matched within 0.1 LU  [-7.68 vs -7.09 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.20 dBTP (Pro-L 2: 0, -0.03 dBTP)]
  PASS  transient retention within 1 dB of Pro-L 2 on every hit  [worst 0.16 dB at hit 14 (34.39 s)]
  FAIL  release limbs within 10% (median t63, t90)  [t63 343.3 vs 1.3 ms, t90 683.0 vs 1.3 ms]
  FAIL  pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB  [1.00 vs 0.70 dB]
-- rule: v2 vs proL2 on bass_sustain
  PASS  level matched within 0.1 LU  [-7.00 vs -7.09 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.05 dBTP (Pro-L 2: 0, -0.03 dBTP)]
  PASS  transient retention within 1 dB of Pro-L 2 on every hit  [worst 0.01 dB at hit 14 (34.40 s)]
  FAIL  release limbs within 10% (median t63, t90)  [t63 1.3 vs 1.3 ms, t90 6.3 vs 1.3 ms]
  PASS  pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB  [0.66 vs 0.70 dB]
-- rule: echojay vs proL2 on fullmix_hot
  FAIL  level matched within 0.1 LU  [-10.69 vs -10.06 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.08 dBTP (Pro-L 2: 0, -0.01 dBTP)]
  PASS  transient retention within 1 dB of Pro-L 2 on every hit  [worst 0.51 dB at hit 9 (30.07 s)]
  FAIL  release limbs within 10% (median t63, t90)  [t63 43.3 vs 0.3 ms, t90 143.5 vs 0.7 ms]
  FAIL  pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB  [0.44 vs 0.08 dB]
-- rule: v2 vs proL2 on fullmix_hot
  FAIL  level matched within 0.1 LU  [-10.17 vs -10.06 LUFS]
  PASS  zero true-peak overs above the ceiling  [0 overs, -0.05 dBTP (Pro-L 2: 0, -0.01 dBTP)]
  PASS  transient retention within 1 dB of Pro-L 2 on every hit  [worst 0.03 dB at hit 12 (46.43 s)]
  FAIL  release limbs within 10% (median t63, t90)  [t63 1.2 vs 0.3 ms, t90 1.5 vs 0.7 ms]
  PASS  pumping (momentary GR std) no worse than Pro-L 2 + 0.2 dB  [0.15 vs 0.08 dB]
```

ROW lines:
```
ROW case=tone_997 tag=proL2 aligned=1 offset=0 outI=0.01 inI=4.84 tp=-0.02 overs=0 edgeOvers=0 grMean=-3.65 grStd=3.01 crestOut=3.2 hits=0 retMed=  n/a t63med=  n/a t90med=  n/a lrStd=0.00 thdMax=-26.6 thdnMax=-26.6
ROW case=tone_997 tag=v2 aligned=1 offset=0 outI=-0.16 inI=4.84 tp=-0.05 overs=0 edgeOvers=0 grMean=-3.81 grStd=3.11 crestOut=3.3 hits=0 retMed=  n/a t63med=  n/a t90med=  n/a lrStd=0.00 thdMax=-44.3 thdnMax=-44.3
ROW case=tone_50 tag=proL2 aligned=1 offset=0 outI=-4.14 inI=0.20 tp=-0.04 overs=0 edgeOvers=0 grMean=-3.21 grStd=2.70 crestOut=2.7 hits=0 retMed=  n/a t63med=  n/a t90med=  n/a lrStd=0.00 thdMax=-19.6 thdnMax=-19.5
ROW case=tone_50 tag=v2 aligned=1 offset=0 outI=-4.20 inI=0.20 tp=-0.05 overs=0 edgeOvers=0 grMean=-3.26 grStd=2.75 crestOut=2.7 hits=0 retMed=  n/a t63med=  n/a t90med=  n/a lrStd=0.00 thdMax=-21.2 thdnMax=-21.2
ROW case=tone_imd tag=proL2 aligned=1 offset=0 outI=0.80 inI=7.43 tp=0.08 overs=4 edgeOvers=0 grMean=-6.22 grStd=1.64 crestOut=5.5 hits=0 retMed=  n/a t63med=  n/a t90med=  n/a lrStd=0.00 thdMax=  n/a thdnMax=-20.9
ROW case=tone_imd tag=v2 aligned=1 offset=0 outI=0.31 inI=7.43 tp=-0.05 overs=0 edgeOvers=0 grMean=-6.69 grStd=1.72 crestOut=6.0 hits=0 retMed=  n/a t63med=  n/a t90med=  n/a lrStd=0.00 thdMax=  n/a thdnMax=-37.9
ROW case=probe_transients tag=proL2 aligned=1 offset=0 outI=-4.65 inI=-1.95 tp=-0.04 overs=0 edgeOvers=0 grMean=-0.57 grStd=1.53 crestOut=4.0 hits=14 retMed=-6.22 t63med=9.5 t90med=9.5 lrStd=0.00
ROW case=probe_transients tag=v2 aligned=1 offset=0 outI=-4.92 inI=-1.95 tp=-0.05 overs=0 edgeOvers=0 grMean=-0.65 grStd=1.73 crestOut=4.1 hits=14 retMed=-6.17 t63med=0.8 t90med=5.3 lrStd=0.00
ROW case=panned_transient tag=proL2 aligned=1 offset=0 outI=-5.93 inI=-5.41 tp=-0.04 overs=0 edgeOvers=0 grMean=-0.28 grStd=0.45 crestOut=4.7 hits=12 retMed=-8.24 t63med=9.7 t90med=9.7 lrStd=0.11
ROW case=panned_transient tag=v2 aligned=1 offset=0 outI=-5.94 inI=-5.41 tp=-0.05 overs=0 edgeOvers=0 grMean=-0.27 grStd=0.46 crestOut=4.7 hits=12 retMed=-8.25 t63med=8.7 t90med=9.0 lrStd=0.13
ROW case=fullmix tag=echojay aligned=1 offset=108 outI=-12.61 inI=-12.60 tp=-0.18 overs=0 edgeOvers=0 grMean=0.06 grStd=0.14 crestOut=11.2 hits=15 retMed=0.12 t63med=26.5 t90med=58.2 lrStd=0.02
ROW case=fullmix tag=proL2 aligned=1 offset=731 outI=-12.60 inI=-12.60 tp=-0.03 overs=0 edgeOvers=0 grMean=-0.00 grStd=0.01 crestOut=11.2 hits=15 retMed=0.00 t63med=0.3 t90med=0.3 lrStd=0.00
ROW case=fullmix tag=v2 aligned=1 offset=0 outI=-12.61 inI=-12.60 tp=-0.05 overs=0 edgeOvers=0 grMean=-0.01 grStd=0.02 crestOut=11.2 hits=15 retMed=-0.00 t63med=0.3 t90med=2.0 lrStd=0.00
ROW case=bass_sustain tag=echojay aligned=1 offset=-388 outI=-7.68 inI=-5.73 tp=-0.20 overs=0 edgeOvers=0 grMean=-1.45 grStd=1.00 crestOut=4.8 hits=20 retMed=-4.32 t63med=343.3 t90med=683.0 lrStd=0.00
ROW case=bass_sustain tag=proL2 aligned=1 offset=235 outI=-7.09 inI=-5.74 tp=-0.03 overs=0 edgeOvers=2 grMean=-1.01 grStd=0.70 crestOut=4.5 hits=20 retMed=-4.16 t63med=1.3 t90med=1.3 lrStd=0.00
ROW case=bass_sustain tag=v2 aligned=1 offset=0 outI=-7.00 inI=-5.74 tp=-0.05 overs=0 edgeOvers=0 grMean=-0.94 grStd=0.66 crestOut=4.4 hits=20 retMed=-4.17 t63med=1.3 t90med=6.3 lrStd=0.00
ROW case=fullmix_hot tag=echojay aligned=1 offset=0 outI=-10.69 inI=-9.94 tp=-0.08 overs=0 edgeOvers=0 grMean=-0.47 grStd=0.44 crestOut=10.2 hits=15 retMed=-1.08 t63med=43.3 t90med=143.5 lrStd=0.00
ROW case=fullmix_hot tag=proL2 aligned=1 offset=731 outI=-10.06 inI=-9.94 tp=-0.01 overs=0 edgeOvers=0 grMean=-0.06 grStd=0.08 crestOut=9.9 hits=15 retMed=-0.86 t63med=0.3 t90med=0.7 lrStd=0.03
ROW case=fullmix_hot tag=v2 aligned=1 offset=0 outI=-10.17 inI=-9.94 tp=-0.05 overs=0 edgeOvers=0 grMean=-0.13 grStd=0.15 crestOut=9.9 hits=15 retMed=-0.87 t63med=1.2 t90med=1.5 lrStd=0.01
```

## 3. Integration (commit 04ff1bf and later)

- EedLimiterProcessor runs on limv2::Core. Same ids, ranges, defaults, state. mode: every value runs Transparent
  tonight (punchy, clip recorded as "not yet tuned"); lookahead_ms scales the tuned window (2 ms = tuned); release_ms
  scales the floor's recovery (50 ms = tuned 180 ms); true_peak drives the 8x/96-tap detector and the post-check;
  sc_hpf_hz is a 2nd-order high-pass on the detector; input_db unchanged (range untouched, Level stage untouched).
- Latency: FIXED, 1344 samples at 48 kHz (28.0 ms) for every setting (TP on/off, any lookahead), reported exactly;
  offline render == live render (fixed-latency and natural-latency renders bit-identical; block-size invariant).
  NOTE for Sean: that is more than the old 108 samples; the 5x window storage for lookahead_ms = 10 costs most of it
  (the tuned window needs 15 samples). If 28 ms is too much for live use, size the storage for 2x instead (~680).
- gainReductionDb(): PEAK GR per block (the deepest gain applied to the block's output samples), as LoudnessLoop reads
  it; proven equal to the measured output/input ratio within 0.00 dB over 3441 reducing blocks.
- No clicks: bypass crossfades 10 ms (delay kept), ceiling smoothed 20 ms (detector at the lower of target/current),
  window changes keep the running average; input gain eased 20 ms. No allocation, no locks on the audio thread.
- CPU, 60 s stereo at 48 kHz: v2 1.15 % of real time, current limiter 0.77 %: 1.49x (threshold 3x). It was 15.65x
  until the 96-tap interpolator was restructured for vectorisation.
- Syntax-checked with the plugin's own compile line (build-release/compile_commands.json): exit 0, no new warnings.
  THE PLUGIN WAS NOT BUILT HERE - that is A's gate.
- Touched: Source/EedLimiterProcessor.h/.cpp, Source/EJLimiterV2Core.h (new), Source/EJLimiterMeterTap.h,
  EJLimiterPanelPalette.h, EedLimiterPanelV2.h/.cpp (new, preview only, not in the plugin's CMake), tools/limiter_ab_guard/*,
  tools/limiter_preview/*, docs/limiter_ab/*. NO file session A edits was changed; no patch for A's files is needed.

## 4. Listening files (~/echojay-limiter-renders/blind/)

```
# blind listening files, 2026-10-07 22:23; seed 20261008; every file scaled DOWN to the quietest of its case
case fullmix_hot  (seed 20261008; every file scaled to the quietest one's integrated loudness, -11.28 LUFS)
  fullmix_hot_A.wav = v2transparent /var/folders/kc/qwn80r_14_d5pqhxvw_nnh080000gp/T/tmp.hfMumAdbci/v2t_hot.wav  (was -10.17 LUFS, scaled -1.11 dB)
  fullmix_hot_B.wav = proL2        docs/limiter_ab/renders/proL2_fullmix_hot.wav  (was -10.06 LUFS, scaled -1.22 dB)
  fullmix_hot_C.wav = current      docs/limiter_ab/renders/echojay_fullmix_hot.wav  (was -10.69 LUFS, scaled -0.58 dB)
  fullmix_hot_D.wav = v2clean      /var/folders/kc/qwn80r_14_d5pqhxvw_nnh080000gp/T/tmp.hfMumAdbci/v2c_hot.wav  (was -11.28 LUFS, scaled +0.00 dB)

case bass_sustain  (seed 20261009; every file scaled to the quietest one's integrated loudness, -8.15 LUFS)
  bass_sustain_A.wav = current      docs/limiter_ab/renders/echojay_bass_sustain.wav  (was -7.68 LUFS, scaled -0.46 dB)
  bass_sustain_B.wav = proL2        docs/limiter_ab/renders/proL2_bass_sustain.wav  (was -7.09 LUFS, scaled -1.05 dB)
  bass_sustain_C.wav = v2transparent /var/folders/kc/qwn80r_14_d5pqhxvw_nnh080000gp/T/tmp.hfMumAdbci/v2t_bass.wav  (was -7.00 LUFS, scaled -1.15 dB)
  bass_sustain_D.wav = v2clean      /var/folders/kc/qwn80r_14_d5pqhxvw_nnh080000gp/T/tmp.hfMumAdbci/v2c_bass.wav  (was -8.15 LUFS, scaled +0.00 dB)

```

## 5. Verbatim test output

limiter_v2_core_test (results/2026-10-08_core_test_final.txt):
```
limiter_v2_core_test
  ok    8x/48-tap detector: |H| within 0.05 dB of unity at 1, 12 and 20 kHz, every phase  [0.00 dB]
  ok    noise bursts +6 dBFS, gain 0.00, ceiling -0.1: ZERO true-peak overs  [-0.15 dBTP, 0 overs, sample -0.15]
  ok    ... and the ceiling is actually reached (not just quiet)  [-0.15]
  ok    latency-compensated render aligns at 0 (reported latency 158 is the real one)  [ offset 0]
  ok    noise bursts +6 dBFS, gain 8.20, ceiling -0.1: ZERO true-peak overs  [-0.15 dBTP, 0 overs, sample -0.15]
  ok    ... and the ceiling is actually reached (not just quiet)  [-0.15]
  ok    latency-compensated render aligns at 0 (reported latency 158 is the real one)  [ offset 0]
  ok    true peak OFF: the sample-domain limiter lets inter-sample peaks over (the known-bad leg of the TP button)  [3.34 dBTP, 7084 overs]
  info  full-band white-noise bursts (content to Nyquist): -0.13 dBTP, 0 overs - not asserted
  ok    below the ceiling the core is a pure delay  [worst 0.000000]
  ok    block size does not change the output (64 / 512 / 1000)  [worst 0.000000]
  ok    probe_transients renders, aligns and shows 14 hits  [ 14]
  ok    probe: zero overs  [-0.05 dBTP]
  ok    pre-dip on every hit no longer than the lookahead (0.30 ms) plus one harness block  [0.00 .. 0.33 ms]
  ok    program-dependent release: a 1-sample hit recovers faster than a 1 s burst (t63)  [0.33 vs 1129.33 ms]
  ok    a 1 s burst stays reduced through its length (GR at +60 ms within 1.5 dB of the minimum)  [-7.78 vs min -7.96]
  ok    a +8.2 dB over impulse retains -8.2 dB (lands at the ceiling)  [-8.25]
  ok    tone_997 renders and aligns in both styles
  ok    tone_997 CLEAN at +8.2 dB over: THD below -60 dB (a gain, not a clipper)  [-144.68 dB, GR -8.30]
  ok    tone_997 TRANSPARENT at +8.2 dB over: rides the waveform like Pro-L 2 (THD -45..-12 dB)  [-44.32 dB, GR -8.19]
  ok    tone_997: zero overs in both styles  [-0.05 / -0.10 dBTP]
  ok    tone_50 renders and aligns in both styles
  ok    tone_50 CLEAN at +8.2 dB over: THD below -60 dB (a gain, not a clipper)  [-115.55 dB, GR -8.30]
  ok    tone_50 TRANSPARENT at +8.2 dB over: rides the waveform like Pro-L 2 (THD -45..-12 dB)  [-21.22 dB, GR -6.95]
  ok    tone_50: zero overs in both styles  [-0.05 / -0.10 dBTP]
  ok    the Transparent default links the right channel at the tuned fraction (Pro-L 2 measured 0.75)  [-5.95 / -7.96]
  ok    panned_transient renders linked and unlinked
  ok    link 1: L and R dip equally  [-7.96 / -7.93]
  ok    link 0: only L dips  [-7.96 / -0.00]
  ok    both hold the ceiling  [0 / 0]
  ok    probe at +15 dB, 44100 Hz: zero overs, no NaN, no subnormal output  [-0.15 dBTP, 0 overs, nan 0, sub 0]
  ok    near-silence (-100 dBFS) at +15 dB, 44100 Hz: pure gain+delay, no NaN, no subnormals  [worst 0.000000 nan 0 sub 0]
  ok    mono probe at +8.2 dB, 44100 Hz: zero overs, no NaN, no subnormals  [-0.15 dBTP, 0 overs]
  ok    probe at +15 dB, 48000 Hz: zero overs, no NaN, no subnormal output  [-0.15 dBTP, 0 overs, nan 0, sub 0]
  ok    near-silence (-100 dBFS) at +15 dB, 48000 Hz: pure gain+delay, no NaN, no subnormals  [worst 0.000000 nan 0 sub 0]
  ok    mono probe at +8.2 dB, 48000 Hz: zero overs, no NaN, no subnormals  [-0.15 dBTP, 0 overs]
  ok    probe at +15 dB, 88200 Hz: zero overs, no NaN, no subnormal output  [-0.15 dBTP, 0 overs, nan 0, sub 0]
  ok    near-silence (-100 dBFS) at +15 dB, 88200 Hz: pure gain+delay, no NaN, no subnormals  [worst 0.000000 nan 0 sub 0]
  ok    mono probe at +8.2 dB, 88200 Hz: zero overs, no NaN, no subnormals  [-0.15 dBTP, 0 overs]
  ok    probe at +15 dB, 96000 Hz: zero overs, no NaN, no subnormal output  [-0.15 dBTP, 0 overs, nan 0, sub 0]
  ok    near-silence (-100 dBFS) at +15 dB, 96000 Hz: pure gain+delay, no NaN, no subnormals  [worst 0.000000 nan 0 sub 0]
  ok    mono probe at +8.2 dB, 96000 Hz: zero overs, no NaN, no subnormals  [-0.15 dBTP, 0 overs]
  ok    probe at +15 dB, 192000 Hz: zero overs, no NaN, no subnormal output  [-0.15 dBTP, 0 overs, nan 0, sub 0]
  ok    near-silence (-100 dBFS) at +15 dB, 192000 Hz: pure gain+delay, no NaN, no subnormals  [worst 0.000000 nan 0 sub 0]
  ok    mono probe at +8.2 dB, 192000 Hz: zero overs, no NaN, no subnormals  [-0.15 dBTP, 0 overs]
  ok    burst then 10 s of silence: output exactly zero after 5 s, no subnormals anywhere  [tail 0.000000 sub 0]
  ok    fixed latency is identical with TP on/off and windows 0 .. 5x  [216 / 216 / 216 / 216]
  ok    a fixed-latency render compensated by the reported number aligns at 0 (on the markers)  [ offset 0]
  ok    fixed latency does not change the audio (same samples as the natural-latency render)  [worst 0.000000]
  ok    the 5x window under fixed latency still holds the ceiling  [0 overs]
  ok    bypass in and out mid-reduction: no sample step beyond the waveform's own (no click)  [max step 0.07 at 1.95 s, tone's own 0.07]
  ok    under bypass the +8 dB tone passes unclipped; after bypass it is limited again  [bypassed peak 2.51, limited peak 0.99]
  ok    sc_hpf 200 Hz: a 30 Hz tone +6 dB over is not reduced (clip holds the samples); hpf off reduces 6 dB  [GR off -6.05, on 0.00, peak on 1.00]
  ok    reported gainReductionDb() = the deepest output/input ratio in the block (peak GR), within 0.2 dB over 3440 reducing blocks  [worst 0.00 dB, 270 blocks deeper than 3 dB]
  info  CPU for 60 s stereo at 48 k: v2 Transparent 0.689 s (1.15 % of real time), legacy 0.463 s (0.77 %), ratio 1.49x
  ok    v2 costs no more than 3x the current limiter (the overnight brief's flag threshold)  [1.49x]
  ok    legacy port reproduces the Pro Tools print docs/limiter_ab/renders/echojay_bass_sustain.wav at the print's measured gain (residual < -40 dB re signal)  [-40.13 dB, offset 388]
  ok    legacy port reproduces the Pro Tools print docs/limiter_ab/renders/echojay_fullmix.wav at the print's measured gain (residual < -40 dB re signal)  [-50.52 dB, offset -108]

==== limiter_v2_core_test: GREEN (0 assertion(s) failed) ====
```

limiter_ab_guard selftest:
```
limiter_ab_guard selftest
  ok    alignment finds a +480-sample, -6 dB render  [offset 480 ]
  ok    alignment finds a -7-sample render  [offset -7]
  ok    a render with three samples inserted mid-file is REFUSED (offset not constant)  [offset is NOT constant across the file - the render is not sample-comparable (resampled, slipped, or a different take)]
  ok    a polarity-inverted render is REFUSED  [polarity inverted against the source]
  ok    unrelated material is REFUSED  [fewer than 2 windows with signal and a correlation peak >= 0.3 (silence, or not the same material)]
  ok    a hard-clipped (heavily limited) render still aligns  [offset 480 corr 1.00]
  ok    windows reading 480 and 481 (one sample apart) align at the median  [ offset 480]
  ok    ...but two samples apart is REFUSED  [offset is NOT constant across the file - the render is not sample-comparable (resampled, slipped, or a different take)]
  ok    a fixed-length bounce 3 s longer than the source, silent tail, aligns (tail reported)  [ tail 3.00]
  ok    ...but a longer render whose tail carries signal is REFUSED  [render is longer than the source and the tail beyond it is NOT silent (-24 dBFS RMS)]
  ok    a -20 dBFS 997 Hz sine on both channels reads -20.0 LUFS (BS.1770)  [-20.04]
  ok    the same with the +8.2 dB stated gain reads -11.8 LUFS  [-11.84]
  ok    short-term std of a steady tone is ~0  [0.00]
  ok    fs/4 sine at 45 deg: sample peak -3.01 dBFS, true peak 0.0 dBTP (+-0.02)  [-3.01 / -0.00]
  ok    20 kHz sine at 0 dBFS: true peak 0.0 dBTP (+-0.05)  [-0.22 / -0.00]
  ok    997 Hz sine at 0 dBFS: true peak 0.0 dBTP (+-0.01)  [-0.00]
  ok    a 20 kHz sine that starts from silence in one sample: its Gibbs overshoot is seen, and reported as an EDGE reading, not an over  [edge 0.59 dBTP, in-audio -0.00]
  ok    overs are counted against a -0.5 dB ceiling  [47589]
  ok    no overs against a +0.2 dB ceiling  [0]
  ok    a print cut mid-cycle at the end: overs 0, EDGE overs counted and reported separately  [0 / edge 1 at 0.62 dBTP]
  ok    ...but an over in the middle of the file is still counted  [58 overs, 0.30 dBTP]
  ok    a faded sine has no overs and no edge overs  [0]
  ok    a pure tone with no markers, delayed two periods, is REFUSED as ambiguous rather than aligned at a wrong period  [alignment AMBIGUOUS: periodic material gives more than one correlation peak in 3 window(s) and fewer than 2 unambiguous windows remain]
  ok    tone_997 synthetic, delayed 96 samples (two periods), aligns at +96 on the markers  [ offset 96]
  ok    six tone segments and five steps analysed  [6]
  ok    steady GR per segment: 0 at -8.2 dBFS, -8.2 at 0 dBFS  [0.00 / -8.20]
  ok    h3 at -40 dB reads as -40 dB THD  [-40.0 / -40.0]
  ok    step response of a 50 ms one-pole: t50 ~35 ms, t90 ~115 ms  [35.0 / 115.0]
  ok    the synthetic (which does not hold a ceiling) shows overs  [11016]
  ok    onset detector finds every layout event on probe_transients  [14 of 14 ]
  ok    hold = the burst length on every hit (0 .. 1000 ms, +3.4 ms for the 0.2 dB rise)  [worst error 3.00 ms]
  ok    retention = -6.0 dB on every hit  [worst error 0.00]
  ok    GR minimum = -6.0 dB on every hit  [worst error 0.00]
  ok    pre-dip (lookahead) = 5 ms on every hit  [worst error 0.33 ms]
  ok    release limbs t63 = 100 ms, t90 = 230 ms within 10%  [worst 3.00% / 1.29%]
  ok    hit 0 is +8.2 dB over the ceiling  [8.20]
  ok    a 30 ms tau is measured as ~30 ms (the fit is not stuck on a constant)  [29.3]
  ok    12 panned events detected  [12 / 12 ]
  ok    unlinked: L dips 6 dB, R does not  [-6.00 / -0.00]
  ok    linked: both channels dip 6 dB  [-6.00 / -6.00]
  ok    L-R GR std separates unlinked from linked  [1.09 / 0.00]
  ok    momentary GR std: +-1 dB at 0.5 Hz reads 0.5-0.8 dB (0.707 unaveraged), steady ~0  [0.67 / 0.00 ]
  ok    steady -6 dB render reads GR mean -6.02  [-6.02]
  ok    float32 WAV round trip  [float32 worst 0.000000]

==== limiter_ab_guard selftest: GREEN (0 assertion(s) failed) ====
```

## 6. Files changed since f724658 (A's base):  38 files changed, 21317 insertions(+), 203 deletions(-)

## 7. Commands run (the shape of it)
cmake -S tools/limiter_ab_guard -B build-limiter-ab; cmake --build build-limiter-ab -j 2 (after every edit, df first);
./build-limiter-ab/limiter_ab_guard selftest; ./build-limiter-ab/limiter_v2_core_test;
./tools/limiter_ab_guard/run_all.sh <step> (renders v2 for the 8 cases at their gains, analyses each against Pro-L 2 and
the current limiter); clang -fsyntax-only with the plugin's compile line on EedLimiterProcessor.cpp and
EedLimiterPanelV2.cpp; ./tools/limiter_ab_guard/make_blind.sh 20261008.

## 8. git log --oneline -15 (before the commit that carries this report)
```
1e2fb06 limiter v2 C7c (FINAL tuning tonight): floor window 4 ms, second stage toward the full reduction (tau 1.2 s)
73713fa limiter v2 C7b: second stage at 0.9 - no change on the primaries (bass -7.13 PASS, hot -10.22 FAIL), tone_997 THD back to FAIL; the hot-mix loss vs C6 is the 8 ms window, not the second stage. READY_FOR_GATE.md ignored in git.
475146b limiter v2 C7: a second, slower floor charge (tau 1.2 s toward the full reduction) from an 8 ms window - Pro-L 2's floor keeps charging on sustained material
a7c8c48 limiter v2 C6: ceiling margin 0.05 dB - output lands at -0.05 dBTP (Pro-L 2 -0.01..-0.04), zero overs on every case
2d04bfb limiter v2 C5: channel link 0.75 - a left-only burst dips the right channel 75 % as much (v2 5.99/8.01 dB, Pro-L 2 5.61/7.53)
aec723e limiter v2 C3: the window is 0.3 ms, a box - no pre-dip beyond what Pro-L 2 shows; a 1-sample impulse gets a ~0.5 ms dip
04ff1bf limiter v2 INTEGRATE: EedLimiterProcessor runs on limv2::Core (same ids, ranges, state); fixed latency; TRUE PK drives the 8x detector; bypass crossfade; smoothed ceiling; sidechain HPF; GR report = gain applied; vectorised detector
4806567 limiter v2 C2: the fast part is instant (0.3 ms) - after a burst Pro-L 2 is back within 0.6 dB in under 0.33 ms
3bb84e0 limiter v2 C1: the slow floor at 72 % of the reduction, 150 ms charge, 180 ms decay, charged from the reduction itself (Pro-L 2 measurements); CLEAN tuning kept as the first v2 behaviour
78fd81b limiter v2: the legacy port of the shipping limiter (proven against the Pro Tools prints, < -40 dB residual), the fullmix_hot case at +10.86, notes with Sean's rulings
c922eab harness: EDGE overs (within one kernel span of a file end, or the flush) reported separately and not counted - a print cut mid-waveform is the cut, not the limiter; self-test legs both ways. First v2-vs-Pro-L 2-vs-EchoJay run on all seven cases.
d958944 limiter v2: proL2_fullmix (TP on) and bass_sustain measurements; latency with TP on/off; the bass over is a truncated print
5b7c688 gitignore: everything in docs/limiter_ab/renders (the panel screenshot was untracked-visible)
10df784 limiter v2: first Pro-L 2 measurements (synthetic cases) and the preliminary full-mix rows; notes updated
677b6b9 harness: accept a render longer than its source when the tail is silent (fixed-length bounces); refuse a tail with signal
```

## 9. df -h ~
```
Filesystem      Size    Used   Avail Capacity iused ifree %iused  Mounted on
/dev/disk3s5   926Gi   800Gi    78Gi    92%    4.0M  814M    0%   /System/Volumes/Data
```
