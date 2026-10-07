# Session L notes — Limiter v2 (Pro-L 2 Transparent target)

Session L works ONLY in ~/echojay-limiter (branch feat/limiter-v2). ~/echojay-vst belongs to session A and is
gated there. Builds: the standalone harness tree only (`cmake -S tools/limiter_ab_guard -B build-limiter-ab`,
`cmake --build build-limiter-ab -j 2`), never -j above 2, never a plugin target, never a plugin loaded.

## State, 7 Oct 2026 (end of day)

Three commits on feat/limiter-v2:
- a67c3f6 the offline A/B harness `tools/limiter_ab_guard` (standalone CMake, no JUCE), fixtures, RENDER_REQUEST.md
- e22cd78 the core `Source/EJLimiterV2Core.h` + `limiter_v2_render` + `limiter_v2_core_test`
- 87aac7d Blackman-Harris spectrum window (THD+N floor -44 -> below -90 dB)

Both test suites GREEN (harness selftest 36 assertions, core test 24). Synthetic baseline of v2 under
`docs/limiter_ab/results/2026-10-07_v2_baseline_synthetic.txt`: -0.10 dBTP, zero overs on all five cases.

NOT yet done:
- wiring the core into EedLimiterProcessor (a plugin build, so it goes through session A's gate; the core is drop-in,
  float arrays, no allocation in process())
- ANY tuning against Pro-L 2: every number in `limv2::Tuning` is a starting point, not a measurement

Blocked on Sean: 8 Pro-L 2 renders + 3 source bounces into `docs/limiter_ab/renders/` (see RENDER_REQUEST.md).
As of 7 Oct evening the folder holds only the five synthetic sources (regenerated 18:49, the final marker version).
Pro-L 2 reference: Default Setting, Transparent, gain +8.2 dB, output 0.0 dB, TRUE PEAK ON (confirmed), oversampling Off.

Rulings:
- the §4 stopping rule is APPROVED as implemented in `compare()` (level 0.1 LU, zero overs, retention 1 dB per hit,
  release limbs 10 %, THD no harmonic > 3 dB worse, pumping no worse than +0.2 dB); the blind listen on fullmix at
  +8.2 dB is the real gate
- do NOT regenerate the source files once Pro-L 2 renders exist: they are the reference the renders were made from
- no tuning of the core without approval; propose Tuning changes first

## The brief for when the renders are in (Sean, 7 Oct)
1. Preflight, verbatim: df -h ~, git status --short, git log --oneline -5, ls -lT of the renders folder; per file
   duration / rate / channels / format; every proL2_<case>.wav newer than its source_<case>.wav, else stop on that case.
2. `limiter_ab_guard analyse docs/limiter_ab/renders --gain 8.2 --ceiling 0.0 --trace`, with v2 renders from
   `limiter_v2_render` (Transparent, TP on) in the same folder, plus echojay_fullmix if present. Save to results/,
   paste ROW and PASS/FAIL lines verbatim.
3. Independent true-peak cross-check with ffmpeg ebur128 peak=true if ffmpeg exists (it does NOT, as of 7 Oct:
   `which ffmpeg` is empty; nothing is to be installed) - say so.
4. Plain-English account of what Pro-L 2 does (lookahead, attack shape, hold, release limbs, linking, tone THD,
   latency), then where v2 differs most, ranked by expected audibility on the hip-hop mix.
5. Propose Tuning changes; do not apply them.

## Why the harness compares renders
Pro-L 2 is PACE-wrapped. `refuseIfPaceWrapped` exists to refuse it, and an unsigned probe cannot load it anyway;
loading raises iLok prompts that took the host down on 1 Oct. So nothing here hosts it: Sean renders, the harness
reads WAVs. Reproducible from files kept in the repo (WAVs git-ignored, results committed).

## Things learned building the harness (each cost a wrong assumption)
- A pure tone is periodic: waveform cross-correlation is ambiguous to a period. The fixtures carry broadband markers
  and the alignment is anchored on them; unanchored ambiguity is detected and refused, not guessed.
- Uniform white noise at -10 dBFS peak has inter-sample peaks ~6 dB above its sample peaks: a true-peak limiter
  DID limit the first markers. They are now Gaussian at -28 dBFS RMS.
- A 48-tap 8x detector under-read full-band noise by 0.5 dB against a 96-tap meter (fractional-delay error near
  Nyquist, not passband). The device's detector is now the same class as its judge - which is exactly why an
  independent cross-check (step 3 above) is owed.
- Hann leakage from a non-bin-centred line put a -44 dB floor under THD+N; Blackman-Harris fixed it.
- The harness marks a dip at -0.5 dB, so a smooth lookahead window measures a pre-dip SHORTER than the lookahead
  (3.0-3.7 ms for 5 ms). The same bias applies to the Pro-L 2 render; compare like with like.

## 7 Oct 2026, evening: first Pro-L 2 measurements (results/2026-10-07_proL2_*.txt)

Renders present: proL2_{tone_997,tone_50,tone_imd,probe_transients,panned_transient} (Logic, 48k float, 38.0 s
fixed-length, compensated: offset 0 on the markers, tails silent), source_fullmix + echojay_fullmix (Pro Tools,
24-bit, uncompensated: echojay offset +108 = 2 ms lookahead + 12 interpolator samples), proL2tpOFF_fullmix (TP OFF
by mistake, offset +496 = Pro-L 2's latency 10.33 ms; NOT a reference, used for GR-envelope shape only).
The five synthetic sources grew by ~9.5 kB on import: Logic appended an `LGWV` overview chunk AFTER the data chunk;
audio verified bit-identical to a fresh generation (never regenerate them now).

What Pro-L 2 Transparent (Default Setting) does, measured:
- NO lookahead pre-dip beyond 0.33 ms: full reduction inside the onset's own block; a 1-sample impulse gets a
  ~0.5 ms-wide dip and lands exactly at the ceiling. Within a burst the gain rides the 3 kHz waveform (+-0.5 dB).
- Release = INSTANT part + slow floor. After a short burst (<= 10 ms) the gain is back within 0.6 dB in < 0.33 ms.
  The floor charges toward ~72% of the required reduction (after 10 ms: 8%, 50 ms: 33%, 200 ms: 66%, 1 s: 72%,
  i.e. one-pole attack tau ~120-185 ms) and decays exponentially in dB with tau ~160-185 ms (measured on every
  limb from 50 ms burst, 200 ms, 1 s, and the tone step to zero: 163/175/183 ms).
- Tones: it rides the waveform (soft-clip-like). THD at +8.2 over: -26.6 dB at 997 Hz (h3 -27, h5 -36), -19.6 dB at
  50 Hz (h3 -20, h5 -31, h7 -40); at +3 over: -30.8 / -25.2; at 0 over (tone at the ceiling): -61.5. Steady GR at
  +8.2 over: 997 Hz -7.97, 50 Hz -6.68 (the 50 Hz tone comes out 1.5 dB louder than a pure gain would give).
  IMD 19+20 kHz: 18k/21k products -24 dB, no 1 kHz product (symmetric).
- Channel linking on a LEFT-only burst: R dips 5.61 dB for L's 7.53 -> ~75% in dB (82% as a linear blend).
- True peak: -0.02..-0.04 dBTP on every clean case; +0.08 dBTP with 4 overs on the IMD pair's hard start.
- Latency 496 samples at 48 k (10.33 ms), from the uncompensated PT render (TP off; TP-on value pending).
- fullmix: on THIS bounce +8.2 dB barely limits: Pro-L 2 (TP off) max GR -0.12 dB, source+8.2 true peak +0.52 dBTP;
  shipping EchoJay max GR -1.35 dB, out -0.18 dBTP (ceiling consistent with 0.0, cannot be -0.3), +0.12 dB louder than
  source+8.2 on every untouched hit (a 0.12 dB gain somewhere in that render), first 50 ms at -0.33 (input-gain ease).
  Either the A/B used a hotter source than this bounce, or the complaint lives in < 1.5 dB of GR on the loudest hits.
  The 1-3 dB "dips" the hit table shows at 46.43/46.90 s are harness noise on near-silent blocks (|in*G| ~ 0.005).

v2 vs Pro-L 2 on the synthetic cases (rule lines in results/2026-10-07_proL2_vs_v2_synthetic.txt): overs PASS,
retention PASS, THD PASS; level FAIL on all five (v2 0.35-1.0 LU quieter), release limbs FAIL (t63 ~100 vs 9.7 ms,
t90 ~700 vs 9.7 ms), pumping FAIL (probe 1.92 vs 1.53 dB, panned 1.00 vs 0.45 dB). Tuning proposals in the report to
Sean of 7 Oct evening; nothing applied.

Independent true-peak cross-check: ffmpeg is NOT installed and neither are numpy/scipy; nothing installed. Still owed.

## 7 Oct 2026, late: proL2_fullmix (TP on) and the bass_sustain trio (results/2026-10-07_fullmix_proL2_echojay.txt,
## 2026-10-07_bass_sustain_gain8.5.txt)

- Latency at 48 k: Pro-L 2 TP off 496 samples (10.33 ms), TP on 731 (15.23 ms): TP adds 235 (4.9 ms). Shipping
  EchoJay 108 (2 ms lookahead + 12). Offsets vs source_bass_sustain: proL2 +235, echojay -388 (source carries 496).
- Gains estimated from unlimited 100 ms blocks (p10 = p90, stable): proL2_fullmix +8.20, echojay_fullmix +8.32,
  proL2_bass +8.49, echojay_bass +8.41 (estimate, Sean to confirm).
- proL2_bass_sustain "+0.22..0.28 dBTP, overs": NOT real - the print ends mid-cycle (last sample +0.905); every
  meter that flushes the end with zeros sees the truncation edge. Inside the audio the max is -0.03 dBTP, 0 overs.
  Independent check: FFT-exact band-limited interpolation (16x zero-padding, no windowed sinc) agrees with the 96-tap
  meter within 0.01 dB on all six real renders. ffmpeg and numpy/scipy absent; nothing installed.
- echojay_* land at -0.18..-0.20 dBTP AND -0.20 dBFS sample: ceiling 0.0 with the wall's 0.1 dB detector margin and
  its 4x/24-tap detector; limiter_wall_guard documents [-0.25, -0.10]. Not a -0.19 ceiling setting.
- fullmix with the real proL2 reference: Pro-L 2 max GR 0.15 dB, EchoJay 1.35 dB; rule lines: level PASS, overs
  PASS, retention PASS (worst 0.60 dB at the one real hit, 30.07 s), release limbs FAIL (26.5/58 ms vs 0.3), pumping
  PASS. On this bounce +8.2 is not a limiting job for either.
- bass_sustain: EchoJay -7.68 vs Pro-L 2 -7.09 LUFS (0.59 LU quieter), GR mean -1.36 (gain-corrected) vs -1.01, max
  -4.1 vs -3.0; time spent reducing > 2 dB: 28 % vs 4 %. Why: (1) Pro-L 2 rides the bass waveform (1 ms GR ripple
  0.43 dB vs 0.14) so it takes ~1 dB less reduction for the same ceiling; (2) EchoJay's one-pole release follows the
  note's own decay from 4.3 dB down (t63 ~340 ms vs Pro-L 2's floor at 72 % + instant part); (3) at every note
  onset EchoJay dips 3.2-3.5 dB where Pro-L 2 dips 1.9-2.1. Linking is not a factor (L-R std 0.00 for both).
- Panel (proL2_panel_bass_sustain.png): Transparent; lookahead just off minimum (~0.3 ms of 0-5) = the <= 0.33 ms
  pre-dip; attack and release both ~11 o'clock on 0-10 s scales (if log, ~150 ms each = the measured 120-185 ms
  floor constants; Sean to hover for the exact value); transient linking ~80 % (measured 75 % dB / 82 % linear);
  release linking 100 %.
