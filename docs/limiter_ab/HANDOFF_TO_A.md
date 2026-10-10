# Hand-off to session A: limiter v2 (feat/limiter-v2) for merging into the main plugin - DRAFT, tests pending

STATUS: NOT READY - the styles round is unbuilt and untested since Sean's stop (A's gate running). This file is completed
with the commit sha and the test results once Sean confirms A is idle and the tests have run. Everything below the
line "TESTS" is what will be filled in.

BRANCH: feat/limiter-v2 (worktree ~/echojay-limiter). Last gated: 7abfa70 (round c GREEN, own_diff re-baselined by ruling).
COMMIT TO MERGE: <pending>

WHAT IS IN IT
- The limiter v2 engine (Source/EJLimiterV2Core.h): true-peak detection (2x half-band + 4x, parabola), held-min
  lookahead with a B-spline window, fast part + two-stage floor fed by a morphological closing, transient and
  release links, post-check stage, fixed latency (1169 samples at 48 k, every setting), smoothed input gain and
  ceiling (20 ms one-pole each, no allocation on change), sample clip, block and peak GR, a meter tap.
- Tunings: transparent() (tuned to Pro-L 2 Transparent, gated), modern() / punchy() / allround() (tuned to the
  Pro-L 2 style prints: hot-mix kick shape matched, bass hold not yet - see SESSION_L_NOTES.md), clean().
- EedLimiterProcessor: schema defaults = Pro-L 2's Default Setting (ceiling_db 0.0, true_peak on, lookahead_ms 0.18
  (0..5), release_ms 400, NEW attack_ms 275 (10..2000), NEW link_pct 75, NEW release_link_pct 100); mode 0..4
  (transparent | punchy | clip | modern | allround; 1 now runs the tuned Punchy, 2 is a Transparent placeholder);
  knob -> Tuning scaling (window = 0.06 ms x lookahead/0.18, floor constants x release/400 and x attack/275, links
  /100); load-time migration (pre-v2 state at lookahead 2.0 AND release 50 -> 0.18/400); a limv2::MeterTap owned by
  the processor. input_db (-12..+12, 0 = unity) is THE IN GAIN for the levelling move: set it with setParamValue /
  applyStructured at any time on any thread that owns parameters, it ramps over 20 ms inside the engine, the meters
  read the gained input; ceiling_db likewise (A holds -0.1; the engine's true-peak margin keeps overs at zero by the
  exact arbiter, 0 of 360 stress configs). Both are plain schema params, no side effects on the other dials.
- EedLimiterEditor: DeviceEditorBase + EedLimiterPanelV2 (header-only): the scrolling display, IN/OUT/GR bars with
  scales, GAIN + CEILING, LUFS column with RESET LUFS, ADVANCED row (LOOKAHEAD, ATTACK, RELEASE, LINK, RLS LINK,
  SC HPF), style menu with the five names. The old transfer-curve face is gone from the limiter.
- Harness: tools/limiter_ab_guard (A/B vs Pro-L 2, arbiter, stress set, core test, sweeps, blind packs),
  tools/limiter_preview (offline panel renders), docs/limiter_ab (notes, results, renders, patches).

FILES THAT OVERLAP WITH A's WORK (A must know)
- NOT touched: ChainHost.*, PluginEditor.*, PluginProcessor.*, CMakeLists.txt source lists (the panel is header-only
  on purpose). CMakeLists.txt carries ONLY the ECHOJAY_TEST_MARKER option (round b).
- EedLimiterProcessor.h/.cpp, EedLimiterEditor.h/.cpp: rewritten (the limiter's own files).
- tools/builtin_registry_test.cpp: the limiter's expectations only (modes 3/4, latency/true-peak legs).
- tools/tests/CMakeLists.txt: the own_diff baseline pin (round c ruling) - and own_diff WILL MOVE AGAIN with the
  styles (the mode line and the device description in the own-channel text): expect a RED, read the diff (limiter
  lines only), rule, re-baseline. The 7abfa70 baseline (OWN_CHANNEL_BASELINE_70f5407.txt) is the last ruled one.
- tools/substitute_guard/harness.cpp: NOT changed here; the patch docs/limiter_ab/patches/2026-10-08_substitute_guard_
  latency_window.patch (the leg's window predates the limiter's latency) must be applied in A's tree.
- Server-side: the AI-facing mode list now has five names; if the server prompt enumerates the limiter's modes it
  needs the same five.

TESTS (to be filled in after Sean's go): limiter_v2_core_test, stress_tp per style (arbiter, 0 of 360 required), the
full comparison against Pro-L 2 (Transparent, byte-identical to the gated C11 renders), limiter_wall_guard (zero
difference per mode, migration, panel legs), loudness_loop_guard, builtin_registry_test, own_diff (expected RED,
limiter lines only). Blind packs per style in ~/echojay-limiter-renders/blind_styles/<style>/ for Sean's listen.
