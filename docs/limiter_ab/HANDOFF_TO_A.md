# Hand-off to session A: limiter v2 (feat/limiter-v2) for merging into the main plugin - 10 Oct 2026

STATUS: READY TO MERGE (styles included; the bass hold of the three styles is a later constants-only update).
BRANCH: feat/limiter-v2 (worktree ~/echojay-limiter). Last gated: 7abfa70 (round c GREEN, own_diff re-baselined by ruling).
COMMIT TO MERGE: the head of feat/limiter-v2 (git rev-parse HEAD; this file cannot name its own commit) = 8ea7ad8 (styles, the last source change) + results-only
  commits (the complete Allround stress file; the suite run and this hand-off). On top of 7abfa70 (round c, gated GREEN).

WHAT IS IN IT
- The limiter v2 engine (Source/EJLimiterV2Core.h): true-peak detection (2x half-band + 4x, parabola), held-min
  lookahead with a B-spline window, fast part + two-stage floor fed by a morphological closing, transient and
  release links, post-check stage, fixed latency (1169 samples at 48 k, every setting), smoothed input gain and
  ceiling (20 ms one-pole each, no allocation on change), sample clip, block and peak GR, a meter tap.
- Tunings: transparent() (tuned to Pro-L 2 Transparent, gated), modern() / punchy() / allround() (tuned to the
  Pro-L 2 style prints: hot-mix kick shape matched within 7 %, bass hold not yet - see SESSION_L_NOTES.md), clean().
  MODE values: 0 transparent (EXACTLY as gated: byte-identical renders), 1 punchy (now the tuned Punchy; before 9 Oct
  it ran the Transparent placeholder), 2 clip (still the Transparent placeholder), 3 modern, 4 allround. Saved chains
  keep their value; no migration needed for mode.
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

TESTS, all on the 8ea7ad8 source, 10 Oct 2026 (files in docs/limiter_ab/results/2026-10-10_*):
- limiter_v2_core_test: GREEN, 67 legs (core_test_styles.txt) - incl. Transparent unchanged by the styles' second
  release (sample for sample), styleTuning (0) == transparent(), each style renders / aligns / holds the ceiling.
- Transparent identity: renders of fullmix_hot, bass_sustain, probe_transients byte-identical (cmp) to the gated
  C11 renders in docs/limiter_ab/renders (v2_*.wav from 70f5407).
- limiter_wall_guard: GREEN (guards_styles.txt): zero difference per mode (the processor at mode 1 / 3 / 4 at the
  defaults == punchy() / modern() / allround(), 0 samples differ, latency 1169 = 1169), the mode-0 zero difference,
  migration, panel legs, measured latency at 44.1 / 48 / 96 k. (One red on the way: Modern's 30 ms erosion was
  clamped to Transparent's ring size inside the plugin; the processor now sizes the rings for every style.)
- loudness_loop_guard: GREEN, leg H -0.28 dBTP (guards_styles.txt).
- builtin_registry_test: ALL PASS, 23 of 23 devices, modern -> 3, allround -> 4, mode range ends at 4
  (builtin_registry_test_styles.txt; run as the guard-tree binary in an isolated HOME: its ctest wrapper builds the
  root build/ tree, absent in this worktree).
- stress_tp (arbiter, 360 configs each): Modern 0 of 360 over, worst -0.00 dB; Punchy 0 of 360, -0.00; Allround
  0 of 360, -0.01 (stress_tp_<style>.txt). Transparent 0 of 360 as gated.
- own_diff: RED by ONE line, reported, NOT re-baselined (own_diff_vs_70f5407.diff): the mode line of the limiter's
  AI-facing text, "mode (transparent|punchy|clip|modern|allround, default transparent) - ...". Written sha
  aef73f2e4ea30e8403d9c50fc6ff0b11fa670993b073c0f79ce05c89aa87eae0 against the ruled 300ee864... (70f5407). A rules
  and pins after the merge, as in round c.
- THE GUARD SUITE (all 49 ej_add_guard targets, built by name on build-guards-lv2; the plugin targets are NOT built
  here; results/2026-10-10_guard_suite_styles.txt): 44 of 49 Passed, 902 s. The five reds, each accounted for:
    own_diff            expected, the one mode line (above); NOT re-baselined.
    substitute_guard    needs A's patch (the leg's window predates the limiter's latency), as in rounds b and c;
                        GREEN here with the patch applied and the file reverted (substitute_guard_with_patch.txt).
    preflight_guard     a PREREQUISITE, not the limiter: it needs the EchoJayProbe binary in build-release, which
                        G builds before its gate (S1) and this worktree does not have (rc 127 on every probe leg);
                        it Passed in G's round c on the same limiter code.
    comp_render_check   LABEL none: "a TOOL, not a test ... carries the label none so no gate runs it" (it also
                        needs the probe). Outside the gate by its written exclusion.
    level_loop_red_prefix  LABEL none: the deliberately PRE-FIX RED reproducer, "registered so it BUILDS with the
                        tree ... with NO label, so no gate runs it". RED is its purpose. Outside the gate.
  So on the gate's own labels (fast|plugins) with the prerequisites G builds: every guard GREEN except own_diff's
  one ruled line and substitute_guard pending its patch - the same two as round c before its ruling.
- Blind packs for Sean's listen: ~/echojay-limiter-renders/blind_styles/<style>/{fullmix_hot,bass_sustain}_{matched,
  unmatched}_{A,B}.wav, KEY.txt in the same folder, every pair the same length (a print's 0.5 s tail used to give the
  key away by file size; blind_pack now pads).
