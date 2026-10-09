# Limiter v2 styles: Modern, Punchy, Allround - the plan (session L, 9 Oct 2026, before the prints)

Three more Tunings of the SAME engine (limv2::Core), our own algorithm, generic names. Nothing here is built or
tuned yet: it is what I expect to move, what the engine may not do, and how MODE and the menu will carry them.
Every number below is a starting guess to be replaced by a measurement; Transparent is the only tuned reference.

## 1. What each style will most likely move (Tuning fields)

The engine's levers, in the order they shape a kick: the window (lookaheadMs, smoothStages: how the gain moves
INTO a hit), the fast part (fastReleaseMs: how it lets go after the hit), the floor (slowFraction, slowAttackMs,
slowReleaseMs, slowWindowMs/slowCloseMs, the second charge slowFraction2/slowAttack2Ms: what sustained level does),
the links (link, linkRelease), and the margins (tpMarginDb, nyquistMarginDb, postMs).

MODERN (expected: Transparent's shape with a tighter, faster-settling floor; a touch more density, less pumping on
sustained material than Punchy)
  slowFraction   0.72 -> ~0.85      the floor takes more of the reduction, so less rides on the fast part
  slowAttackMs   150  -> ~80-120    the floor charges faster (a hot mix settles within a bar)
  slowReleaseMs  180  -> ~120-180
  slowCloseMs    10   -> ~6-8       a shorter bridge: tighter between hits
  fastReleaseMs  0.05 -> ~0.1-0.2   a slightly slower let-go (the "modern" smoothness after the hit)
  lookaheadMs    0.06 -> 0.06-0.10
  Expect: kick dips a little shallower than Transparent at +3/+8 ms, between-hit mean deeper (-0.15..-0.25 dB), bass
  level within 0.1 LU of its print, pumping std lower than Transparent's on bass_sustain.

PUNCHY (expected: transients pass, the body is held: the fast part lets go at once, the floor is deep and quick)
  lookaheadMs    0.06 -> 0.03-0.06  (a shorter window lets the attack through; 0 is the engine's floor)
  fastReleaseMs  0.05 (already the minimum that makes sense)
  slowFraction   0.72 -> ~0.9-1.0   the floor carries almost the whole sustained reduction
  slowAttackMs   150  -> ~40-80     and charges fast, so a kick pops over a held-down body
  slowReleaseMs  180  -> ~200-300   and stays down between hits (the "pumping" Punchy is known for)
  slowCloseMs    10   -> ~12-20     a longer bridge: the floor holds across the gap between hits
  link           0.75 -> ~0.5-0.75  (measured on panned_transient: Pro-L 2's styles may link differently)
  Expect: deepest kick dips of the three (retention closest to the source), between-hit mean the deepest, bass level
  possibly LOUDER than its print if we do not hold the floor long enough, and the most pumping.

ALLROUND (expected: close to Transparent, a little more sustain reduction, a little less kick dip; the safe middle)
  slowFraction   0.72 -> ~0.6-0.8
  slowReleaseMs  180  -> ~250-400   a longer, steadier recovery
  slowAttackMs   150  -> ~150-250
  lookaheadMs    0.06 -> ~0.08-0.12 a slightly rounder attack
  slowCloseMs    10 (unchanged unless the bass print says otherwise)
  Expect: everything within ~20 % of Transparent; the tone and probe rules should pass as they do now.

The knobs keep scaling the chosen style's constants the way they scale Transparent's (applyLookahead): RELEASE
scales slowReleaseMs by label/400, ATTACK scales slowAttackMs and slowAttack2Ms by label/275, LOOKAHEAD the window
by label/0.18, LINK and RLS LINK the two links. So each style's Tuning is what the processor runs AT the defaults,
and the zero-difference test (processor at the defaults == the style's Tuning) is repeated per style.

## 2. What the engine might not do yet (to be decided by the prints, not guessed)

(a) DRIVE / COLOUR. If a style's tone_997 THD rises in a way gain-riding cannot explain (odd harmonics well above
    Transparent's at the same reduction), that is a waveshaping element - the engine has no soft clipper, only the
    sample clip at the ceiling. Adding one is engine work (a sample-domain soft knee under the ceiling, before the
    post-check so the arbiter still sees zero overs) and would be its own measured step, not a Tuning. Punchy is
    the candidate.
(b) PRE-DIP LONGER THAN THE WINDOW. Pro-L 2's lookahead and its smoothing are one window in our engine. If a style
    shows a pre-dip that starts earlier than its dip is wide (the harness's pre(ms) much longer than the gain's
    rise), we need a lookahead separate from the smoothing (a held-min horizon longer than the B-spline support).
    Engine work, small.
(c) RELEASE THAT DEPENDS ON THE MATERIAL BEYOND TWO STAGES. The floor has a first and a second charge with fixed
    constants. If a style's t63 differs by more than the harness's resolution between probe_transients (isolated
    hits), bass_sustain (LF pulses) and fullmix_hot (dense programme) in a way no single (fast, floor) pair fits,
    the release constant itself depends on the signal (crest factor or recent GR). The measurements will say; the
    engine change would be a release constant blended by a crest-factor estimate.
(d) STYLE-DEPENDENT LINKING. If panned_transient shows a different R dip per style, link and linkRelease carry it;
    if it shows a different SHAPE (R dips later or recovers differently), that is per-channel smoothing - engine work.
(e) Nothing frequency-dependent is expected (no sidechain filtering in Pro-L 2's styles); tone_50 vs tone_997 will
    confirm.

## 3. MODE: how the styles are exposed (saved chains unchanged)

Today: mode is a choice 0..2 = transparent | punchy | clip (kNumModes 3). Since 8 Oct every value runs the Transparent
tuning; punchy and clip "keep their dial value, recorded as not yet tuned".

Proposal:
  0 transparent  UNCHANGED: exactly the gated Tuning, the zero-difference test stays as it is.
  1 punchy       BECOMES the tuned Punchy. The saved value and its name keep their meaning; what changes is that a
                 chain saved with mode 1 stops running the Transparent placeholder and runs what its name says.
                 THIS IS THE ONE JUDGMENT CALL in the plan: it is the only way "punchy" means punchy without a second
                 punchy value. If Sean prefers that chains saved with 1 keep sounding as they do now, Punchy goes in
                 as a new value instead and 1 stays a placeholder (I recommend against: a mode called punchy that
                 is Transparent is a wrong label, and no chain was saved with mode 1 by a v2 build yet).
  2 clip         UNCHANGED (still the placeholder; a hard-clip style is not in this brief).
  3 modern       NEW.
  4 allround     NEW.
  kNumModes 3 -> 5; the schema's choice list "transparent|punchy|clip|modern|allround"; the AI-facing description
  gains one clause per style. Old chains: any saved mode value loads literally (0..2 exist today); a chain saved
  with no mode loads 0. No migration needed. The processor picks the base Tuning by mode in applyLookahead
  (`styleTuning (mode_)`), then applies the knob scaling exactly as now.

Consequences to flag now:
  - own_diff WILL GO RED again (the mode line and the device description change). Same procedure as round c:
    G reads the diff, Sean rules, I re-baseline with the sha pinned.
  - tools/builtin_registry_test.cpp: the limiter's mode range expectation (max 2 -> 4) - shared file, limiter
    expectations only, exact diff listed in the hand-off.
  - The server's prompt/classifier may name the limiter's modes; not this session's file - flagged for the server
    session if its text enumerates them.

## 4. The UI's style menu

EedLimiterPanelV2's style box: "Transparent", "Punchy", "Clip", "Modern", "Allround" (item id = mode + 1, so the
order follows the schema values; the menu is already bound to "mode" and reads back from the processor). The PNG
set is re-rendered once per style at the defaults for the record.

## 5. When the prints arrive (renders/styles/<style>/{probe_transients,tone_50,tone_997,bass_sustain,fullmix_hot}.wav)

Gains as printed: +8.2 (probe, tones), +8.5 (bass), +10.9 (fullmix_hot) - note 10.9, not the 10.86 the Transparent
comparison used; the style comparisons use 10.9 for both Pro-L 2 and ours, and I will say so in the results.
Per style, in this order, all offline (the harness tree only, -j 2, df first):
  1. Preflight each print: format 48 k float, length vs its source, alignment by the harness (median offset, ambiguity
     refused), NOT regenerating any source. The prints are laid into the harness's naming by symlink
     (renders/styles/<style>/proL2_<case>.wav -> the print; source_<case>.wav -> the shared source).
  2. Measure Pro-L 2's style against the source like Transparent: kick shape (GR at +1/+3/+8/+20 ms, t63, between-hit
     mean) on fullmix_hot, bass level / limbs / pumping, probe retention and limbs, THD on tone_997 and tone_50,
     linking on panned_transient (if printed), overs by the arbiter. Written up before any tuning, as the target.
  3. Tune: a Tuning per style from the guesses in §1, swept with kick_sweep*.sh / kick_summ.py against the targets,
     the same §4 rule lines, each step committed with its numbers. If §2 finds engine work, it is reported first.
  4. Overs by the arbiter on the full 360-config stress set per style: zero, or the style is not done.
  5. Core test legs per style (renders, aligns, holds the ceiling, the zero-difference at its defaults), guards as in
     round c (touched ones while iterating, full set before the hand-off).
  6. Blind pack per style: ~/echojay-limiter-renders/blind_styles/<style>/ - Pro-L 2's print and ours, level-matched
     (integrated LUFS equal), A/B named by a random key, the key in a separate file (not printed in the report),
     LUFS of every file reported. make_blind.sh's pattern.
  7. STOP for Sean's listen. No gate, no READY_FOR_GATE, until the listen is in.
