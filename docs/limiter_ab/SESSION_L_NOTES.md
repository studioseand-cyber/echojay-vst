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

## 7 Oct 2026, night: rulings, the hot full mix, the legacy port (results/2026-10-07_v2_vs_proL2_vs_echojay_all.txt,
## 2026-10-07_fullmix_hot_gain10.86.txt)

Rulings from Sean: A is NOT gating (build -j 2 in own dir, render allowed). echojay_bass_sustain was printed at
EchoJay input gain 16.7 = +8.5 nominal vs source_bass_sustain; Pro-L 2 hovered values Attack 275.0 ms, Release
400.0 ms (lookahead as read: just off minimum, ~0.3 ms). The knob labels do NOT equal the measured constants (floor
charge 120-185 ms, decay 160-185 ms): tune to the measurements, not the labels. fullmix_hot: Pro-L 2 dial 16.7,
EFFECTIVE gain vs source_fullmix +10.86 dB (coordinator: unlimited blocks = source x 10.86 dB, residual -108 dB; my
own estimate +10.86, p10/p90 10.85/10.86); analysed at --gain 10.86 for all three tags, v2 and the current limiter
rendered at +10.86 from source_fullmix (source_fullmix_hot.wav is a symlink to source_fullmix.wav).

The bass print's gain, measured: the legacy port reproduces echojay_bass_sustain best at +8.41 (residual -40.1 dB)
and worse at +8.5 (-38.9); echojay_fullmix at +8.32 (-50.5 dB) vs +8.2 (-37.7). In unlimited blocks the wall is at
unity, so the 0.1 dB is NOT the wall margin: the prints carry +0.12 (fullmix) and -0.09 (bass) of gain somewhere
upstream of the limiter's own gain. Small, but it is why "level matched" is judged on the Pro-L 2 render's own
unlimited blocks, never on the dial.

THE LEGACY PORT (tools/limiter_ab_guard/ejlegacy.h, limiter_legacy_render): EedLimiterProcessor::processBlock
(transparent, TP on, lookahead 2, release 50, the 50 ms multiplicative input-gain ease from 1.0) on the JUCE-free
headers it already uses. Proven by limiter_v2_core_test against the two Pro Tools prints (residual < -40 dB re
signal, -40.1 and -50.5); skipped with a message where the renders are absent. Latency 108 at 48 k, like the print.

Harness: EDGE overs (within 96 samples of a file end or in the flush) are reported separately and not counted.

fullmix_hot (+10.86, hits up to 4.5 dB over by the hit detector): Pro-L 2 out -10.06 LUFS (0.12 LU under
source+gain), max GR 0.96 dB on 400 ms blocks, release 0.3-2 ms; current EchoJay -10.69 (0.6 LU under Pro-L 2),
max GR 2.78, t90 ~140 ms; v2 -11.28 (1.2 LU under), max GR 3.23, t90 up to 900 ms. All three hold the ceiling
(0 overs; -0.01 / -0.08 / -0.10 dBTP). Retention on the hardest hit (30.07 s, +4.5): Pro-L 2 -4.58, EchoJay -5.09,
v2 -4.78 - all three land the peak at the ceiling; the difference is entirely what happens AROUND the hit.

## 8 Oct 2026, overnight run (Sean's approvals of 7 Oct, late): TUNE, INTEGRATE, listening files, hand-off

Rulings recorded: EchoJay bass print dial 16.7 = +8.5 nominal (measured effective +8.41, see 7 Oct late); Pro-L 2
hovered Attack 275.0 ms, Release 400.0 ms - NOT the measured constants (floor charge 120-185 ms, decay 160-185 ms);
tuned to the measurements. fullmix_hot at --gain 10.86 (coordinator: unlimited blocks = source x 10.86 dB,
residual -108 dB). A was not gating; builds at -j 2 in build-limiter-ab only; df checked before every build (78 GB).

Tuning steps, each a commit with its rule lines (results/2026-10-07_C*.txt):
  C1 slow floor 72 % / 150 ms charge / 180 ms decay / 1 ms source window  bass -8.15 -> -7.57 LUFS (Pro-L 2 -7.09; current -7.68)
  C2 fast part instant (0.3 ms)                                          bass -7.40, hot -10.39 (current -10.69; Pro-L 2 -10.06)
  C3 window 0.3 ms, box                                                  bass -6.92, hot -10.12 PASS; pumping PASS on both; tones now ride
  C5 link 0.75                                                           panned dips 5.99/8.01 (Pro-L 2 5.61/7.53); limbs rule flips by 1 ms
  C6 ceiling margin 0.05                                                 (see results)
  C4 is a consequence of C2/C3 (riding), approved; CLEAN keeps the first behaviour as limv2::clean().
Open after C6: tone THD - 50 Hz clipped 4.5 dB deeper than Pro-L 2 (h3 -15.5 vs -20.0), 997 Hz clipped less (h3 -37 vs
-27) with higher-order harmonics from the box window; probe pumping 1.74 vs 1.53; levels on the tone cases. The
measurements say Pro-L 2's floor keeps charging on sustained material (72 % after 1 s, ~97 % after 4 s at 997 Hz,
81 % at 50 Hz) and holds across an LF cycle - a second, slower charge (tau ~1.2 s toward 100 %) from an ~8 ms window.
Proposed as C7, run as its own step, kept only if it is better on the primaries.

INTEGRATE (commit 04ff1bf): EedLimiterProcessor on limv2::Core, same ids/ranges/state; mode -> Transparent for every
value (punchy, clip not yet tuned); lookahead_ms scales the window (2 = tuned), release_ms scales the floor's
recovery (50 = tuned 180 ms); fixed latency 1344 samples at 48 k (5x window storage, TP on; identical for TP off and
every dial); GR report = the gain applied to the block's output samples (peak GR), proven 0.00 dB; CPU 1.13 % of
real time vs the current limiter's 0.76 % (1.48x) after vectorising the 96-tap interpolator (15.65x before).
Syntax-checked with the plugin's own compile line (build-release/compile_commands.json): exit 0, no new warnings.
The plugin itself is NOT built here (A's gate).

The current limiter at any gain = ejlegacy.h, the port of EedLimiterProcessor's old path, proven against the Pro
Tools prints (-40.1 / -50.5 dB residual) by the core test.

### Tuning outcome, 8 Oct early morning: C7c is the version to gate
  C7  8 ms floor window + second charge (tau 1.2 s to 100 %)   bass PASS -7.13 | hot -10.22 (0.16 off) | all tone THD PASS
  C7b second stage at 0.9                                     no change on the primaries; 997 THD back to FAIL - reverted
  C7c 4 ms window, second stage at 100 %  (FINAL)             bass PASS -7.00 | hot -10.17 (0.11 off, target 0.10) | tone_50 level
                                                              PASS and h3 -21.8 vs Pro-L 2 -20.0 | all THD PASS | pumping PASS everywhere
Missed target, stated: fullmix_hot level is 0.11 LU under Pro-L 2 (0.10 allowed). The 1 ms floor window that recovers it
(C6, -10.11 PASS) puts the bass 0.21 LU too loud and fails two THD rules; 4 ms is the best split found. The release-limb
rule fails on t90 (6.3 vs 1.3 ms on bass, 1.5 vs 0.7 on the hot mix): 10 % of a millisecond is under the harness's
0.33 ms block; t63 matches (1.3 vs 1.3, 1.2 vs 0.3). The three tone-level FAILs (997 Hz 0.17 LU, imd 0.49, probe 0.27)
are sustained synthetic material where our floor settles deeper than Pro-L 2's; not primaries.
Minimum bar for the hand-off (closer than the current limiter on level and pumping, both primaries): met.
  bass_sustain  level  current -7.68 | v2 -7.00 | Pro-L 2 -7.09      pumping  current 1.00 | v2 0.66 | Pro-L 2 0.70
  fullmix_hot   level  current -10.69 | v2 -10.17 | Pro-L 2 -10.06   pumping  current 0.44 | v2 0.15 | Pro-L 2 0.08
Transparent tuning (limv2::transparent()): lookahead 0.3 ms box, fast part 0.3 ms, floor 72 % with 150/180 ms, window
4 ms, second charge to 100 % with tau 1.2 s, link 0.75, margin 0.05 dB, post-check 1 ms. CLEAN = the first behaviour.

## 8 Oct 2026, day: the gate's RED (session G) and the arbiter round

G's gate on 10fe9d3: RED. Build clean; limiter_wall_guard overs (+0.83 dBTP by its 4x/24 meter, +1.11 by exact
reconstruction) on the 1 s +6 dBFS noise burst; latency 216 (not the 1344 the hand-off said - that number came from the
core test's pre-C3 configuration, 5 ms x 5; 216 was right for the 0.3 ms window); builtin_registry_test asserting the
old lookahead+12 formula and comparing one block in place.

ARBITER (tools/limiter_ab_guard/ejdsp.h truePeakExact): exact band-limited reconstruction, FFT zero-padding 16x in 64k
chunks with 50 % overlap, parabola through each local maximum. Shares nothing with any windowed-sinc meter. Validated on
full-scale sines 997 Hz..23.9 kHz at three phases within 0.015 dB (the residual at 23.9 kHz is the test fade's own
sideband folding at Nyquist: 0.18 dB with a 10 ms fade, 0.04 with 100 ms, 0.01 with 300 ms). It decides every overs
result now (margin +0.02 dB); the 96-tap meter is printed alongside.

THE PHYSICS: a windowed sinc of half-length M under-reads full-band white noise by about 0.45/sqrt(M): 96 taps 0.54 dB,
1024 taps 0.20 dB (measured against the arbiter on the limiter's own output); 0.02 dB would need M ~ 38000 samples.
No real-time detector reads white noise exactly; music has nothing at Nyquist and reads exactly with M ~ 500.

THE FIX (EJLimiterV2Core.h): (1) TruePeakHB - a 2x half-band Kaiser interpolator with half-length 512 (every other
tap zero: 1024 MACs per sample), then a 32-tap 4x stage on the 2x stream, then a parabola over the 8 points per sample;
used by the main detector and the post-check. (2) B-spline gain windows instead of boxes (the box's steps put the
limiter's own products at Nyquist; same 0.3 ms support, so the tuning is unchanged). (3) A Nyquist-band margin: the
gained input's first-difference energy fraction (0 for music, 1 for white) above 0.05 scales an extra margin of up to
0.30 dB. Cost: latency 1103 natural / 1160 fixed samples at 48 k (24.2 ms; was 216), CPU 4.65 % of real time (5.9x the
current limiter's 0.79 %; was 1.5x). Stress set (stress_tp, 360 configs per limiter: guard bursts, white, pink, square
100/1000 Hz, tones 20/22/23.5 kHz, hard-clipped mix, hot mix; +6/+10/+15; ceilings 0/-1; 44.1/48/96 k; mono/stereo):
before the fix v2 Transparent +1.26 dB worst, CLEAN +0.48, the CURRENT LIMITER +1.60 (+1.94 at 96 k, 252 of 360 configs
over) - the old limiter had this all along and its guard's meter could not see it; after the fix v2 Transparent +0.02
worst, 0 of 360 over (the last six, pink at 44.1 k mono +0.03/+0.04, needed the margin onset lowered from 0.10 to 0.05).

GR for the loop: gainReductionDb() is now BLOCK GR (output energy / gained-input energy), which is what the loop's
estimate measures (J5 read estimate 2.27 vs peak GR 3.51); gainReductionPeakDb() feeds the meter's needle. Both
measured in the core test.

Guards: limiter_wall_guard judged by the arbiter, latency reported == measured impulse delay (44.1/48/96 k, TP on/off,
2/10 ms), transparency with the delay compensated. builtin_registry_test: only the limiter's expectations changed
(results/2026-10-08_builtin_registry_test_limiter.diff). loudness_loop_guard untouched.

CMake: option ECHOJAY_TEST_MARKER (OFF): appends -lv2test to both products' names and to the Info.plist version strings
via PLIST_TO_MERGE; the numeric VERSION (JucePlugin_VersionCode) is untouched.
Late additions (8 Oct, midday): the Nyquist-band margin ended at 0.60 dB (0.30 satisfied the arbiter, but
loudness_loop_guard's leg H judges white-noise bursts with a 4x/24-tap meter that over-reads HF by up to 0.33 dB and
must pass as it stands); gainReductionDb() became BLOCK GR (energy ratio) for the loop, gainReductionPeakDb() for the
meter (J5: estimate 2.32 vs real 2.36); the loop restructure briefly dropped the bypass crossfade's update line (caught
by the core test's bypass leg, restored). Final: core test GREEN (57), stress 0 of 360 over for both v2 styles,
limiter_wall_guard / loudness_loop_guard / builtin_registry_test all PASS on build-guards-lv2 (arm64, no LTO).

## 8 Oct 2026, 14:12: gate round b (session G) on 6870d4f - RED BY ONE GUARD, substitute_guard
58 of 59 passed; everything red in round a is GREEN (limiter_wall_guard, loudness_loop_guard, builtin_registry_test).
G's own arbiter check on the built marker-ON archive: GREEN (-0.47..-1.00 dBTP on the bursts). The one red is an
untouched guard's window: tools/substitute_guard/harness.cpp (session A's file) asserts a 0.1 impulse leaves above 0.2
INSIDE its own 512-sample block, and the limiter now delays 1160 samples; the gain is applied (G measured 0.232 at
+1160, 0.237 expected). Fix: drain silent blocks until the reported latency has passed and take the peak - written as
docs/limiter_ab/patches/2026-10-08_substitute_guard_latency_window.patch, proven here (substitute_guard Passed on
build-guards-lv2 with it applied), NOT applied in this branch: A's file. The gate applies it like the gate-tools patch.
Also from the report: the -lv2test bundles share the installed build's identifiers, so Logic sees one component (the
name and UUIDs tell them apart); place_ship.sh hard-codes the non-marker names (G placed by hand).
