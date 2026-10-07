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
