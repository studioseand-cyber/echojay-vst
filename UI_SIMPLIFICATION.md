# UI SIMPLIFICATION TO MATCH ANTARES (5 Sep 2026) - the rulings, the semantics, THE INVENTORY TO RULE ON

Sean's ask, after confirming the pitch work in Logic: too many controls;
make it exactly like Antares. Depth automatic. Retune reading 0-400 with
Antares' effect. A Natural Vibrato knob.

## Ruling 1 - THE RETUNE RE-MAP IS NOW CORRECT; round 31's decline is SUPERSEDED, not reversed silently
Round 31 declined re-mapping the dial because "a re-mapped dial cannot
provide gentleness at retune 44, Sean's working point, below the knee by
construction." That objection assumed OUR dial semantics. Under an
Antares-style dial, 44 is no longer his gentle setting - 300-400 is.
Changing what the dial MEANS dissolves the objection. Design: ONE control,
0-400 ms, driving retune speed AND depth together along a CALIBRATED
curve; depth becomes internal to the curve. The round-31 depth parameter
survives underneath (ruling 4) and its measurements stand.

## Ruling 2 - CALIBRATE AGAINST MEASUREMENT, NOT ASSUMPTION
Requested from Sean (ANTARES_RETUNE_SWEEP_REQUEST.md, on his Desktop and
in the repo): five Antares bounces of the same vocal, identical settings,
only Retune Speed varying - 0, 50, 100, 200, 400 - with the provenance
line per the standing rule. Then our (retune_speed_ms, depth) pair is
FITTED at each of their five dial positions so that activity, off-grid,
improve-rate and word-start events match theirs; the fitted curve and
its residuals are reported per position, and a position that cannot be
matched is named with the size of the miss. UNTIL THOSE BOUNCES EXIST
THE CURVE IS NOT GUESSED: interpolating between their 0 and 400 assumes a
shape we have no evidence for. Known anchors today: their max (400)
measures 0.57c activity / improve 57.0%; ours at retune 150 depth 25
measures 1.84c / 58.0% (round 31). That is one end, not a curve.

## Ruling 3 - NATURAL VIBRATO IS NOT THE IGNORE-VIBRATO BUTTON (corrected in the record and to Sean)
Antares has both, separately, and so do we:
  - NATURAL VIBRATO scales the vibrato already present - a gain on the
    deviation-from-target; theirs runs -12 (reduce) .. +12 (exaggerate),
    0 = as sung.
  - TARGETING IGNORES VIBRATO is a DETECTOR control: it stops wide
    vibrato confusing target selection (ours: targeting_ignores_vibrato,
    ON in every mode).
(ONSET_SHAKINESS_RESEARCH.md section 4; the Auto-Tune Pro X guide.)

OUR natural_vibrato (0-200, default 100), SEMANTICS AS SHIPPED - measured
before any mapping, per the ruling:
  The corrector forms shiftCents_ = shiftSm_ + (k - 1) * osc with k =
  natural_vibrato / 100 - a gain on the singer's deviation, 0 dead-still,
  100 as sung, 200 exaggerated - BUT that shift reaches the shifter only
  on the SHIFT PATH, which shiftPreferred() selects only when k is within
  0.5 of 1.0. At every other value the shifter runs the legacy target/f0
  path, whose target is the ENVELOPE (the note track) with no osc term,
  so the wobble is removed entirely. Measured, retune 44 and 6, ign OFF:
  natural_vibrato 0, 40, 150 and 200 render BIT-IDENTICALLY (activity
  6.09c at 44); only 100 differs (1.84c - the vibrato kept):
##### vibrato survival (tools/pitch_vibdepth: output 4-8 Hz cents-modulation depth as a ratio of the source's, on sustained notes) #####
-- natvib 0 at retune 44: /private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/nv/self_44_60_0_0_0_0.wav: notes 0 | depth ratio med 0.00 | rate src 0.00Hz out 0.00Hz | src depth med 0.0c 
-- natvib 40 at retune 44: /private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/nv/self_44_60_0_0_0_40.wav: notes 0 | depth ratio med 0.00 | rate src 0.00Hz out 0.00Hz | src depth med 0.0c 
-- natvib 100 at retune 44: /private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/nv/self_44_60_0_0_0_100.wav: notes 1 | depth ratio med 0.96 | rate src 6.00Hz out 6.00Hz | src depth med 41.9c 
-- natvib 200 at retune 44: /private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/nv/self_44_60_0_0_0_200.wav: notes 0 | depth ratio med 0.00 | rate src 0.00Hz out 0.00Hz | src depth med 0.0c 
-- natvib 0 at retune 6: /private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/nv/self_6_60_0_0_0_0.wav: notes 0 | depth ratio med 0.00 | rate src 0.00Hz out 0.00Hz | src depth med 0.0c 
-- natvib 100 at retune 6: /private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/nv/self_6_60_0_0_0_100.wav: notes 1 | depth ratio med 0.97 | rate src 6.00Hz out 6.00Hz | src depth med 41.9c 
-- natvib 200 at retune 6: /private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/nv/self_6_60_0_0_0_200.wav: notes 0 | depth ratio med 0.00 | rate src 0.00Hz out 0.00Hz | src depth med 0.0c 

  (Reading the ruler: "notes 0 / ratio 0.00" on the non-100 renders means
  the ruler found no sustained note with 4-8 Hz modulation on the OUTPUT
  side to qualify - i.e. none survived; at 100 the same source notes
  qualify at a survival ratio of 0.96-0.97 at 6 Hz. The bit-identity of
  the 0/40/150/200 renders is the primary evidence; the ruler agrees.)
  SO: the control is BINARY today - 100 keeps the vibrato, anything else
  removes it. The schema's "above 100 exaggerates" is false as shipped.
  The presets: natural/balanced 100 (keep), tuned 40 and hard 0 (remove).
  Earlier observations agree (round 17: natvib 40 == natvib 0; round 28:
  the forced shift path at k = 0 is worse than legacy at hard - the fast
  term's phase caveat, "11 clicks and 13.1c at k = 0").

PROPOSED MAPPING (second, as ruled; not built): Antares -12..+12 <->
ours 0..200 with 0 <-> 100, IF AND ONLY IF the gain actually applies at
every value. That requires the vibrato gain to leave the path-selection
rule: either route every k through the shift path with the RING-ALIGNED
fast term (the dbgFastRing experiment - the fast component applied by
the engine per sample from its own ring, phase-correct by construction)
measured clean at k = 0 and k = 2, or apply (k - 1) * osc on the legacy
path as a co-timed decision-ring quantity. Bar for that, before any
knob: vibdepth survival ratio ~0.0 at 0, ~1.0 at 100, ~2.0 at 200 on the
synthetic vibrato note and on sourceNEW's sustains, with the round-28
click and 13.1c figures at k = 0 not reproduced. Until then a Natural
Vibrato knob would be a switch with 24 positions that only two of do
anything.

## Ruling 4 - SIMPLIFY BY HIDING, NOT DELETING
Front panel: the Antares-equivalent set. Advanced panel: engineering-
facing controls. Reasoning: folding depth into the dial removes "fast
timing, gentle amount" as a combination - Antares cannot do that either,
so it is part of matching them - but the capability survives underneath.
seam_attack_ms is a FIX, not a taste control; prior art exposes it (GSnap
Attack, Waves Note Transition) but it does not belong on the front
panel: keep the parameter, hide the control. Nothing is deleted.

## THE CONTROL INVENTORY - every current parameter, for ruling (layout follows the set)

  parameter                    today                        proposed    Antares equivalent
  key_root                     KEY control                  FRONT       Key
  scale                        SCALE control                FRONT       Scale
  key_source                   auto/manual button           FRONT       (Antares: manual key only; "auto" has no equivalent - keep as the way back to auto, small)
  retune_speed_ms              0-150 ms knob                FRONT       Retune Speed 0-400 - RE-MAPPED (ruling 1), drives depth along the calibrated curve
  flex                         FLEX knob 0-100 %            FRONT       Flex-Tune (theirs 0-100)
  humanize                     HUMAN knob 0-100 %           FRONT       Humanize (theirs 0-100)
  natural_vibrato              no control (0-200 %)         FRONT       Natural Vibrato -12..+12 - ONLY after ruling 3's fix; until then ADVANCED
  targeting_ignores_vibrato    button                       FRONT       Targeting Ignores Vibrato (button)
  reference_hz + reference_source   REF knob + auto button  FRONT       Detune (Hz reference; theirs has no auto - the auto button is ours, keep small)
  correct                      master enable button         FRONT       (bypass / Mix 0 in theirs; keep as the on/off)
  transpose                    no control                   FRONT       Transpose (semitones) - theirs is front-panel; ours has no UI yet
  depth                        DEPTH knob (round 31)        ADVANCED    no equivalent as a control (internal to their dial) - folded into retune; survives underneath
  seam_attack_ms               no control                   ADVANCED    no equivalent (prior art: GSnap Attack, Waves Note Transition) - a fix, hidden
  voice_type                   control                      ADVANCED    Input Type (theirs is front-panel; ours moves back only if the auto-suggest readout is not enough)
  tracking                     control                      ADVANCED    Tracking (theirs is a front-panel knob 0-100; ours is a 3-choice strictness) - ADVANCED unless ruled otherwise
  formant_mode                 control                      ADVANCED    Formant on/off (theirs front-panel) - ours preserve in every mode; ADVANCED
  formant_shift                no control                   ADVANCED    Throat Length (theirs) - ours carries a measured fidelity cost; ADVANCED
  low_latency                  button                       ADVANCED    Low Latency mode (theirs is a mode switch)
  correction_mode              natural/balanced/tuned/hard  ADVANCED    no equivalent (presets in theirs) - the mode table stays as presets, hidden
  mix                          no control                   ADVANCED    Mix (theirs front-panel; the chain wet knob covers ours)
  output_db                    no control                   ADVANCED    no equivalent
  ref_manual_by_user           internal flag                INTERNAL    none - provenance (DEFECT_GUARD_WITHOUT_MIGRATION)
  vib_depth_cents / vib_rate_hz / vib_shape / vib_onset_ms   no controls   ADVANCED   Create Vibrato section (theirs: Rate, Onset Delay, Onset Rate, Pitch/Amplitude/Formant Amount) - ours is a generator block with no panel
  target_hz                    P1 fixed-target diagnostic   INTERNAL    none
  reset_stats                  momentary                    INTERNAL    none
  (readouts, not params: detected-key line, voice-fit suggestion, reference provenance line - ADVANCED panel, always visible in it)

Not in the inventory because they are not parameters: the DEPTH-under-
the-dial curve (ruling 2's fit), the auto-key path's guards (shipped),
the adopt-on-engage rules (parked backlog).

## Parked, unrun, unchanged by this phase
Broader material validation; adopt-on-engage; the eighteen constants;
DEFECT_VIBRATO_ON_TUNING_COST; the mid-retune Antares bounce (now
subsumed by ruling 2's five-bounce sweep).

# Round 34 (5 Sep 2026): the finding generalises - VERIFY BEFORE PLACING; the flagged calls; the vibrato knob's precondition

## STANDING RULE (ruling A): A PARAMETER'S DOCUMENTED BEHAVIOUR IS A CLAIM, NOT A FACT, UNTIL A RENDER SHOWS IT.
natural_vibrato was a continuous parameter that was actually binary, with
a false schema description and shipped presets sitting at "remove
entirely". If one parameter's documented behaviour was fiction, THE
INVENTORY CANNOT BE BUILT ON NAMES. Before any parameter is placed FRONT
(and for every ADVANCED parameter a user could reasonably reach) it is
verified by the method that caught this one: render at several values
across its range, confirm the renders differ, confirm the direction of
change matches the description, report every one that does not.
Instrument: EchoJayPitchModeTest with EJ_VERIFY_OUT=<dir> renders the
standing take through a FRESH processor via the real setParamValue path
at min / mid / max / default (numerics), every choice (choice params),
both states (booleans), on a D-minor-by-hand base; the offline rulers
(pitch_activity, pitch_key_forensic, pitch_vibdepth, RMS, alignment lag)
then report differs / direction. The table follows in round 35.

## Ruling B - the flagged calls
  VOICE TYPE -> FRONT. Antares has Input Type on its front panel, and this
  investigation's own evidence says getting it wrong costs accuracy:
  every probe pinned low_male while Sean ran alto_tenor for a month, and
  DEFECT_VOICE_TYPE_DEFAULT is open. A control that is costly when wrong
  does not get buried.
  TRACKING -> ADVANCED for now. Antares has it on the front, but ours is
  our detector's parameter, not theirs, and after the vibrato finding its
  semantics are unverified. Promoted to FRONT only if verification shows
  it behaves as described AND maps meaningfully onto Antares' Tracking.
  TRANSPOSE -> verify before giving it any control. An unexposed parameter
  has never been exercised by a user; there is no reason to assume it
  works. If it does, FRONT (Antares has it). If it does not, file it and
  leave it unexposed.

## Ruling C - THE NATURAL VIBRATO KNOB DOES NOT SHIP UNTIL THE GAIN APPLIES
A 24-position control where two positions do anything is worse than the
button it replaces. The fix is the fast term reaching the audio at every
value - the ring-aligned hybrid ruled in PATH_UNIFICATION_DECISION.md and
never built, whose original corrector-side attempt produced +7 clicks
because (k-1)*osc was hop-sampled and applied a third of a vibrato cycle
late. THE CONVERGENCE, recorded: THE CO-TIMED DECISION RING BUILT THIS
WEEK (TIMING_ALIGNMENT_RECORD flags A/E) IS EXACTLY THE PHASE-CORRECT
CARRIER THAT ATTEMPT LACKED - a per-sample, position-indexed quantity
read beside f0Here at the read pointer. (k-1)*osc written into that ring
is applied at the audio's own instant, not a hop late. It is real DSP
work, scoped with its own bar before building (the bar sketched in
ruling 3: vibdepth survival ~0.0 / ~1.0 / ~2.0 at 0 / 100 / 200 on the
synthetic vibrato note and on sourceNEW's sustains; the round-28 "+7
clicks / 13.1c at k = 0" not reproduced; bit-identical at 100).
Until then natural_vibrato STAYS THE BUTTON IT EFFECTIVELY IS, and the
schema description is CORRECTED NOW to say so (done, this commit): 100
keeps, everything else removes; presets tuned (40) and hard (0) both
mean REMOVE. SEAN HAS BEEN TOLD his tuned and hard presets remove all
vibrato - a product fact he needed regardless of the UI work.

## Ruling D - the retune curve waits on the five bounces; the request is on his Desktop. Not guessed.

## Order for this phase: verification table -> revised inventory -> rule on it -> layout. No UI code until the inventory is ruled. The pitch backlog stays parked.

# Round 35 (5 Sep 2026): THE VERIFICATION TABLE - every parameter rendered before it is placed

Method (ruling A): EchoJayPitchModeTest with EJ_VERIFY_OUT renders sourceNEW
through a fresh processor at the schema defaults (the NATURAL preset: retune
120, flex 55, humanize 60, natural_vibrato 100) with D minor set by hand,
then once per (parameter, value) through the real setParamValue path:
min / mid / max / default for numerics, every choice, both boolean states.
105 renders. Offline: samples differing from the default render, RMS, and
alignment lag (python); activity + tuning (pitch_activity, pitch_key_forensic
with the source), sustains (pitch_sustain_tuning), the generator's size by
render-vs-default activity, direct ACF f0 for transpose, and channel-exact
comparison for bypass. Run log: tools/pitch_mode_test/verify_2026-09-05/.

TWO CAVEATS ON THE INSTRUMENT, found while using it:
  - sourceNEW is STEREO (L-R at -12 dB re L). The harness renders CHANNEL 0;
    every offline ruler averages L+R. Pitch rulers do not care; sample-exact
    checks must compare against channel 0 (the bypass "residual" of -18 dB
    was exactly this and nothing else).
  - The NATURAL-preset base hides small effects: flex 55 leaves +-55c of
    deviation alone, so the reference and scale changes barely show at
    natural; they were re-verified at hard (retune 6, flex 0) where they
    show in full. Verification bases must be stated with the verdict.

  parameter                  values rendered                    differs?   direction vs description                                                verdict
  correct                    0 / 1                              yes        0 = bypass: SAMPLE-EXACT copy of channel 0 delayed 1800 (37.5 ms)       VERIFIED
  correction_mode            5 choices                          yes        natural = default; custom = keep; balanced/tuned/hard differ            VERIFIED (presets act)
  retune_speed_ms            0 / 75 / 120 / 150                 yes        activity rises with tau at natural (1.21 -> 1.68c); full sweep round 17  VERIFIED
  flex                       0 / 50 / 55 / 100                  yes        more flex = less tuning (improve 46.5 -> 44.9%), as described            VERIFIED (and: >= 25 tunes worse than dry, round 27)
  humanize                   0 / 50 / 60 / 100                  yes        sustain off-grid 4.8 -> 5.1 -> 5.6c: relaxes sustains, small            VERIFIED
  depth                      0 / 1 % (flag bug) + rounds 27-31   yes        0 = identity, 25/50/100 measured (round 31)                               VERIFIED
  natural_vibrato            0 / 1 (flag bug) + round 33 sweep  yes        BINARY: 100 keeps, all else removes; description was false (corrected)   VERIFIED AS A SWITCH
  targeting_ignores_vibrato  0 / 1                              yes        OFF differs; detector-side (round 4/vibrato defect file)                  VERIFIED
  key_root                   12 roots                           mostly     A and D identical (A minor = D minor on the degrees this take visits)    VERIFIED; material-limited
  scale                      11 choices                         mostly     minor = default; custom = keep; dorian and harmonic minor identical to  VERIFIED; material-limited
                                                                           minor (they differ only on B/Bb and C/C#, unvisited here); chromatic differs
  key_source                 auto / manual                      yes        auto follows an external G-major fact (F -> F#); manual = default        VERIFIED
  reference_hz               380 / 440 / 500 (+415/430/450/466) yes        grid offset -39.5c @430, +37.3c @450; 380/500 behave as a grid modulo a   VERIFIED (schema range = engine clamp 380-500)
                                                                           semitone (-1.7c @415, +20.4c @500); hidden at natural, shown at hard
  reference_source           auto / manual                      no         identical with no source and field 440 - expected; the state tests cover it   VERIFIED elsewhere (round 24)
  transpose                  -12 / 0 / +12                      yes        -12: 164.4 -> 82.8 Hz at every probe (level -3.7 dB); +12: 326.5/331 Hz at   WORKS WITH A DEFECT - not FRONT-ready
                                                                           3.15/4.05 s but 179.8 Hz at 2.55 s (a ~155c shift where 1200c was asked)
  voice_type                 5 choices                          yes        all differ; LATENCY changes per type (+816 low_male, +3968 bass, -992 soprano)   VERIFIED (a fit-to-material control; latency reported)
  tracking                   relaxed / normal / tight           yes        tight -> more untracked hops (178 vs 162 relaxed), improve 46.3 vs 44.2%   VERIFIED as a strictness; NOT Antares' Tracking
  formant_mode               off / preserve / shift             partly     OFF == PRESERVE bit-identically: within the 2.5-st splice band the resampler   VERIFIED WITH A FINDING
                                                                           does no formant work, so the switch only acts on large shifts; SHIFT differs and drops level 2.4 dB even at 0 st
  formant_shift              -12 / 0 / +12 (in shift mode)      yes        RMS -1.7 / -2.4 / -3.0 dB; timbre direction not measured                    VERIFIED differs; direction unmeasured
  low_latency                0 / 1                              yes        output arrives 608 samples (12.7 ms) EARLIER                               VERIFIED
  mix                        0 / 50 / 100                       yes        0 = dry (sample-exact), 50 = -0.4 dB sum, 100 = default                    VERIFIED
  output_db                  -24 / 0 / +24                      yes        exactly -24.0 / +24.0 dB                                                  VERIFIED
  seam_attack_ms             0 / 1 (flag bug) + rounds 15-27    yes        0 vs 60 measured at every tau (calmer)                                     VERIFIED
  vib_depth_cents            0 / 50 / 100                       yes        generated vibrato size 7.4c / 15.1c median (scales with depth)             VERIFIED
  vib_rate_hz / vib_shape / vib_onset_ms   sweeps at depth 30   yes        rate 10 Hz 4.6c; onset 3000 ms -> 0.44c (vibrato never arrives on short notes)   VERIFIED
  target_hz / reset_stats / ref_manual_by_user   not rendered   -          diagnostic / momentary / provenance flag                                   INTERNAL

## FINDINGS FROM THE TABLE (beyond the vibrato switch)
  1. THREE CONTINUOUS PARAMETERS WERE FLAGGED AS BOOLEAN in the schema:
     natural_vibrato (0-200), seam_attack_ms (0-150), depth (0-100, copied
     from seam). The flag advertises "on/off switch" to every schema
     reader (the model, the dashboard, the harness - which rendered them
     at 0 and 1). Corrected to false in this commit. The editor was
     unaffected (its knobs ignore the flag) - which is why nobody saw it.
  2. THE DEFAULT PRESET TUNES WORSE THAN THE DRY SOURCE ON THIS TAKE:
     natural (retune 120, flex 55, humanize 60) - improve-rate 44.6%,
     all-voiced off-grid 7.95c vs the source's 6.72c. Consistent with the
     round-27 Flex finding (>= 25 tunes worse than dry). A fresh device
     on a solo vocal at its shipped default makes the tuning worse. For
     ruling with the inventory: the default preset is a product decision.
  3. formant_mode OFF and PRESERVE are the same sound for every correction
     under 2.5 semitones (the splice band) - i.e. for all pitch-correction
     use. The switch only means something on large shifts (transpose,
     formant_shift); on the front panel it would be a control that does
     nothing. ADVANCED.
  4. transpose +12 has a regional failure (2.55 s: 179.8 Hz where ~329 was
     asked - a splice-band-sized shift instead of an octave) and -12 loses
     3.7 dB. Verify-first was right: it does not get a control until this
     is filed and fixed. Filed here; DEFECT_TRANSPOSE_OCTAVE.md is the
     follow-up when the phase allows.
  5. Verification on ONE take cannot discriminate scales or roots that
     differ only on degrees the take never visits (dorian, harmonic minor,
     A minor all render identically to D minor here). Not a defect; a
     limit of the material, to be stated wherever those rows are cited.

## THE REVISED INVENTORY (for ruling; layout follows)
  FRONT:     key_root (Key), scale (Scale), key_source (auto/manual, small - the way back to auto),
             retune_speed_ms (re-mapped to 0-400 once the five bounces calibrate it - ruling 2),
             flex (Flex-Tune), humanize (Humanize), targeting_ignores_vibrato (button),
             reference_hz + reference_source (Detune, with the auto button small),
             correct (on/off), voice_type (Input Type - ruling B),
             natural_vibrato AS THE TWO-STATE CONTROL IT IS ("keep vibrato" on/off; the knob waits on ruling C's DSP)
  ADVANCED:  depth (survives under the dial), seam_attack_ms (a fix), tracking (ours, not theirs),
             formant_mode + formant_shift (no effect under 2.5 st; shift mode drops level),
             low_latency, correction_mode (presets), mix, output_db, the create-vibrato block,
             transpose (until its defect is fixed; then FRONT)
  INTERNAL:  ref_manual_by_user, target_hz, reset_stats
  READOUTS (ADVANCED, always visible there): detected-key line, voice-fit suggestion, reference provenance line
  FOR RULING WITH THE INVENTORY: the default preset (finding 2); whether "keep vibrato" as a labelled
  switch belongs FRONT until the knob exists; where the DEPTH knob goes while the re-map waits on the bounces
  (today it is FRONT beside HUMAN, shipped in 6fcbb0a).

# Round 36 (5 Sep 2026): three rulings and two notes; the default preset changes NOW

## Ruling 1 - THE DEFAULT PRESET CHANGES NOW, MARKED PROVISIONAL
A pitch corrector whose shipped default DETUNES is broken as a product,
and it is the first thing every new user hears. natural at retune 120 /
flex 55 / humanize 60: improve-rate 44.6%, off-grid 7.95c against the
source's 6.72c (round 35), corroborating round 27 (flex >= 25 tunes worse
than dry at retune 44) - two independent measurements implicate flex on
this material. NEW DEFAULT: correction_mode custom at retune 44, flex 0,
humanize 0, depth 100, seam_attack 60 - the measured-safe neighbourhood,
where Sean works and what he has ear-confirmed. Schema defaults,
PitchCorrect member defaults and the mode default changed together (one
default per parameter). The four character presets are unchanged and
selectable.
THE CAVEAT, carried honestly: this take is already autotuned and therefore
near-grid and hard to improve; on genuinely raw material flex 55 might
help. The new default is PROVISIONAL and is re-derived when the broader
takes land (SEAM_ATTACK_VALIDATION_REQUEST.md). But a demonstrably
harmful default does not ship on the hope that other material rescues it.
ON THE RECORD: this may be the origin of Sean's opening complaint - "the
echojay pitch sounded awful on this vocal" - since a fresh instance at
the shipped default is what he would have heard first, and the shipped
default made this vocal's tuning worse than leaving it alone.

## Ruling 2 - KEEP-VIBRATO GOES FRONT, HONESTLY LABELLED
A two-state control whose label says so: "KEEP VIBRATO", on/off. NOT
"Natural Vibrato" while it is a switch - naming a switch after Antares'
continuous knob invites exactly the confusion that produced round 33.
When the gain ships (ruling C's DSP, on the co-timed decision ring) it
takes the same slot and earns the name then. THE NAME IS RESERVED, NOT
WITHHELD.

## Ruling 3 - DEPTH STAYS FRONT UNTIL THE RE-MAP EXISTS
Sean asked for depth to be automatic and it will be, but the re-map waits
on five Antares bounces that do not yet exist. Depth is the control that
solved his problem and that he uses daily. A working control is not
removed to satisfy a simplification that is not built. It moves to
ADVANCED in the same change that ships the re-mapped dial, not before.

## Note A - formant_mode does nothing in practice
OFF and PRESERVE are bit-identical for every shift under 2.5 semitones -
all pitch-correction use. Recorded plainly: THE CONTROL DOES NOTHING IN
PRACTICE. Proposal for the ruling: not ADVANCED by habit - INTERNAL, no
control, until a shift-mode use (transpose, formant_shift) exists on the
front, at which point the switch means something and returns with it.

## Note B - the copy-paste metadata hazard (filed beside the one-default rule in ONSET_PASS_RECORD)
depth copied seam_attack_ms's schema entry and inherited its `true` in
the boolean slot; natural_vibrato had the same. A new parameter added by
copying an existing entry INHERITS ITS METADATA, and metadata errors are
invisible because the editor's knobs ignore the flag - only schema readers
(the model, the dashboard, the verification harness) see them.

## Transpose: stays unexposed; DEFECT_TRANSPOSE_OCTAVE.md filed. VERIFY-FIRST IS VINDICATED:
an unexposed parameter that had never been exercised turned out to be
broken at +12 in one region (155c where an octave was asked, 2.55 s) and
to lose 3.7 dB at -12. It would have shipped as a front-panel Transpose
on the strength of its name.

## The material-limited rows carry their caveat wherever cited
dorian, harmonic minor and A minor render identically to D minor on
sourceNEW because the take never visits B/Bb or C/C#. That is a
limitation of the take, not a finding about the parameter. Any citation
of those rows says so.

Order unchanged: revised inventory -> rule -> layout. No UI code yet. The
retune re-map waits on the five bounces; the pitch backlog stays parked.

INSTALLED (5 Sep 2026, provisional default + corrected schema flags + corrected natural_vibrato text; suite 161/0):
    AU   arm64 9EDF2852-B117-3155-8501-ECEE79465E8F
    VST3 arm64 F655DD95-FE93-3887-BA86-9FD267B34C56
  Sean's session is unaffected (his saved params restore explicitly: retune 44.2 / flex 0 / humanize 0 already);
  a NEW instance now opens at custom 44 / 0 / 0 / depth 100 / seam 60. Relaunch Logic before judging anything.
