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

# Round 37 (5 Sep 2026): THE FIVE ANTARES BOUNCES - re-measured from Sean's files, the fit, the dial mapping

## STANDING RULE (ruled): ALIGN QUASI-PERIODIC SIGNALS BY ENVELOPE, NOT BY WAVEFORM CORRELATION, OR VERIFY THE WAVEFORM LAG AGAINST AN ENVELOPE LAG.
Waveform cross-correlation on a pitched voice locks onto whole pitch
periods and returns a different lag per window (this machine, three
0.5 s windows per file: -238/-112/-170 ... -268/-60/-232 samples - period
multiples of ~290). Envelope correlation cannot lock onto a period.
THE RE-CHECK of every Antares alignment cited in this record, by envelope:
    file                                    cited lag        envelope lag   verdict
    antares_retune0_NEW (round-4 ANT_0)     +252 (glitch tool)  +288        fine: this tool is envelope-coarse then waveform-refined within +-64 (< a quarter period)
    antaresNEW (round-4 ANT_400)            +28                 +32         fine
    antaresmaxretune (round 17)             +6                   0          fine
    antares3 (27 Aug, the retune-44 clip's Antares leg)   +3996  -6720     WRONG: +3996 was a clamped search-window edge (+-4000); the true
                                                                            offset is -6720 (140 ms). The round-31 retune-44 ear clip's Antares
                                                                            leg was misaligned by 223 ms - a sequential listen, so not destructive,
                                                                            but wrong. That clip is DELETED and re-cut below against 50.wav.
    the five (0..400)                        container 0 all    +288, +32, +32, +32, +32   0.7-6 ms: irrelevant to hop-level rulers; the shape is unaffected
The container's "+1714" for ANT_0 (round 4) was the container's own ruler; this machine never carried it.

## THE FIVE, this ruler vs the container (sourceNEW reference; ours: pitch_activity / pitch_key_forensic / pitch_glitch_events)
    retune | activity med (ours / cont) | off-grid (ours / cont) | <5c (ours / cont) | improve (ours / cont) | ws events ours (of total)
       0   |   5.46 / 5.63              |  1.55 / 2.04           |  76.4 / 66.4      |  83.0 / 76.6          |  4 of 12
      50   |   2.44 / 2.50              |  6.21 / 6.57           |  43.0 / 42.5      |  57.4 / 58.0          |  3 of 10
     100   |   1.75 / 1.87              |  6.41 / 6.56           |  41.0 / 42.1      |  57.1 / 58.1          |  4 of 13
     200   |   1.14 / 1.18              |  6.53 / 6.66           |  40.4 / 40.8      |  56.0 / 57.0          |  3 of 11
     400   |   0.70 / 0.75              |  6.53 / 6.77           |  40.2 / 40.0      |  57.6 / 57.0          |  3 of 12
AGREEMENT: activity within 0.12c everywhere; off-grid/improve within 0.5c / 1.5 points above 0; at retune 0 this ruler reads tighter (1.55 vs 2.04c, 76 vs 66% <5c) - the same tracker-window difference round 4 recorded (here vs container: 1.94 vs 1.15 then, opposite sign; the two rulers bracket it). THE SHAPE IS THE SAME ON BOTH: authority collapses between 0 and 50 (off-grid 1.55 -> 6.21 against the source's 6.72), then PLATEAUS 50 -> 400 (6.21 -> 6.53) while activity keeps falling (2.44 -> 0.70). Above ~50 Antares is not correcting; it is getting quieter about not correcting. And: Antares carries 3-4 WORD-START glitch events at every setting on this ruler (round 11 saw 13 events at ANT_0); our seam fix holds 0-1.

## THE FIT (our grid: retune {6,20,44,80,150} x depth {1 .. 0.05}, hard base, ign OFF, seam 60, foundation on; full grid in tools/pitch_activity/logs_2026-09-03/fit_grid_2026-09-05.txt)
    Antares dial | ours (retune_ms, depth) | activity ours/theirs | off-grid ours/theirs | <5c ours/theirs | improve ours/theirs | ws ours/theirs
        0        | (6, 1.00)               |  4.49 / 5.46         |  2.33 / 1.55         |  69.7 / 76.4    |  79.6 / 83.0        |  0 / 4
       50        | (80, 0.35)              |  2.34 / 2.44         |  6.37 / 6.21         |  42.5 / 43.0    |  60.2 / 57.4        |  0 / 3
      100        | (150, 0.25)             |  1.84 / 1.75         |  6.75 / 6.41         |  40.3 / 41.0    |  58.0 / 57.1        |  0 / 4
      200        | (150, 0.15)             |  1.13 / 1.14         |  6.96 / 6.53         |  39.3 / 40.4    |  59.1 / 56.0        |  1 / 3
      400        | (150, 0.10)             |  0.76 / 0.70         |  6.80 / 6.53         |  39.2 / 40.2    |  60.3 / 57.6        |  1 / 3
RESIDUALS: activity within 0.1c at 50-400; off-grid within +0.16..+0.43c (ours a shade looser on the plateau); <5c within 1.1 points; improve-rate ours +1..+3 points HIGHER at the same activity (we improve the grid slightly more per cent of movement); word-start events ours 0-1 vs theirs 3-4 (cleaner).
THE POSITION THAT CANNOT BE MATCHED: ANTARES 0. Our fastest, fullest setting (retune 6, depth 1) is 0.8c looser off-grid (2.33 vs 1.55), 6.7 points fewer within 5c, 3.4 points lower improve-rate, and 1.0c LESS active. That is the round-4 onset-accuracy gap, still open, now bounded on this ruler. It is not a depth or speed question; nothing in the grid reaches it. Stated, not smoothed.
WHAT CARRIES THE MATCH: DEPTH. Above 50 the retune value matters little (retune 44 with depth 0.35/0.25/0.15/0.10 gives 2.13/1.51/0.92/0.61c - also within reach); the fitted retune rises so that "slower" stays true on our dial, and the middle anchor (150 / 0.25 <-> Antares 100) was the round-31 correspondence the ruling said to start from - it holds: 1.84 vs 1.75c, 58.0 vs 57.1%.

## THE 0-400 DIAL MAPPING, proposed (piecewise-linear between the five anchors; STEEP below 50, near-FLAT above)
    dial   0 ->  retune   6 ms, depth 1.00
    dial  50 ->  retune  80 ms, depth 0.35      (between 0 and 50: retune 6->80 and depth 1.00->0.35, linear in dial)
    dial 100 ->  retune 150 ms, depth 0.25
    dial 200 ->  retune 150 ms, depth 0.15
    dial 400 ->  retune 150 ms, depth 0.10
Below 50 the map is the whole of the authority collapse (depth 1.0 -> 0.35 in 50 dial units); above 50 depth drifts 0.35 -> 0.10 over 350 units while retune sits at our cap. A LINEAR DEPTH ACROSS 0-400 WOULD BE WRONG and is not proposed. Sean's ear-confirmed working points sit on the curve: his retune 44 / depth 1.0 (today's default) is NOT on it - it is a fast-and-full point Antares' dial does not offer (their 0 is faster and their 50 is shallower); the re-map makes that point unreachable from the dial and reachable only from the ADVANCED depth knob (ruling 4, hide not delete). Stated for the ruling.
NOT BUILT: the re-map waits on the ruling; the curve is derived from measurement, not assumed.

## The retune-44 ear clip RE-CUT against the mid-retune bounce that now exists
  /Users/SeanD/echojay-vst-pitch/earclips/depth_retune44__A_source__B_antares_retune50__C_echojay_retune44_depth50.wav
  3 legs x 13.25 s, 0.45 s gaps, jointly normalised; Antares 50 aligned by envelope (+32) and at the source's level (+0.0 dB, no gain);
  the 27 Aug proxy clip is deleted (its Antares leg was misaligned by 223 ms).

# Round 38 (5 Sep 2026): the dial's FULL TRANSFER FUNCTION under ruling 1, measured at thirteen positions

## Ruling 1 - DO NOT REPRODUCE ANTARES' MISSING MIDDLE (a DELIBERATE DEVIATION, not an approximation of the fit)
Their dial has no intermediate region: 0 gives 1.55c off-grid, 50 gives
6.21c against the source's 6.72, and nothing exists between. Sean's
working setting - retune 44 at FULL depth, shipped as the provisional
default in round 36 and ear-confirmed - is a fast-and-full point their
dial cannot express. Copying the gap would delete a setting he uses daily
to replicate a coarseness with no use in it. MAPPING: dial 0 to 40 holds
DEPTH AT 1.0 while retune varies 6 -> 44 ms; above 50 the fitted taper
exactly (depth 0.35 -> 0.10, retune at the cap). At 0 and at 50-400 our
numbers mean what theirs mean; between, we offer a usable region they do
not. Sean has been told and can overrule toward strict identity.

## THE TRANSFER FUNCTION - exact (retune_ms, depth) at every dial position
Piecewise-linear in dial between six knots:
    dial   0 : (  6 ms, 1.000)
    dial  40 : ( 44 ms, 1.000)      <- the fast-and-full region ends here (Sean's default)
    dial  50 : ( 80 ms, 0.350)      <- Antares 50, fitted
    dial 100 : (150 ms, 0.250)      <- Antares 100, fitted (the round-31 anchor)
    dial 200 : (150 ms, 0.150)      <- Antares 200, fitted
    dial 400 : (150 ms, 0.100)      <- Antares 400, fitted
Between knots both coordinates interpolate linearly in dial units, so:
dial 10 = (15.5, 1.0); 20 = (25, 1.0); 30 = (34.5, 1.0); 45 = (62, 0.675);
75 = (115, 0.30); 150 = (150, 0.20); 300 = (150, 0.125). The 40 -> 50
segment carries the whole authority collapse (depth 1.0 -> 0.35 in ten
dial units) - by design, it is where Antares' 0 -> 50 cliff lives.

## MEASURED ACROSS ITS LENGTH (sourceNEW, hard base, ign OFF, seam 60, foundation on; tools/pitch_activity/logs_2026-09-03/transfer_function_2026-09-05.txt)
    dial | (retune, depth)  | activity med  p75    p90   >25c | off-grid  <5c   improve | ev / ws   | Antares at this dial (activity / off-grid / improve / ws)
       0 | (6, 1.00)        |   4.49   8.09  15.59   3.7 |  2.33   69.7   79.6  | 1 / 0     |  5.46 / 1.55 / 83.0 / 4
      10 | (15.5, 1.00)     |   5.27  10.70  21.71   8.5 |  2.67   63.6   70.4  | 3 / 1     |  -
      20 | (25, 1.00)       |   5.46  12.99  26.38  10.6 |  3.35   58.2   66.8  | 4 / 0     |  -
      30 | (34.5, 1.00)     |   5.70  13.96  28.65  11.7 |  3.75   55.8   63.8  | 5 / 0     |  -
      40 | (44, 1.00)       |   6.09  14.58  30.16  13.3 |  4.26   53.5   61.4  | 7 / 0     |  -   (the provisional default; ear-confirmed)
      45 | (62, 0.675)      |   4.46  10.78  23.88   9.3 |  5.71   46.5   59.5  | 4 / 0     |  -
      50 | (80, 0.35)       |   2.34   5.70  13.19   2.4 |  6.37   42.5   60.2  | 4 / 0     |  2.44 / 6.21 / 57.4 / 3
      75 | (115, 0.30)      |   2.12   5.14  12.18   1.7 |  6.54   41.4   58.0  | 3 / 0     |  -
     100 | (150, 0.25)      |   1.84   4.52  10.67   1.0 |  6.75   40.3   58.0  | 3 / 0     |  1.75 / 6.41 / 57.1 / 4
     150 | (150, 0.20)      |   1.50   3.66   8.42   0.6 |  6.86   39.5   58.2  | 1 / 0     |  -
     200 | (150, 0.15)      |   1.13   2.74   6.21   0.4 |  6.96   39.3   59.1  | 3 / 1     |  1.14 / 6.53 / 56.0 / 3
     300 | (150, 0.125)     |   0.94   2.26   5.13   0.4 |  6.87   39.3   59.6  | 2 / 1     |  -
     400 | (150, 0.10)      |   0.76   1.80   4.14   0.2 |  6.80   39.2   60.3  | 2 / 1     |  0.70 / 6.53 / 57.6 / 3
WHAT THE CURVE DOES, plainly: from 0 to 40 the dial slows the correction
at full depth - tuning loosens from 2.33 to 4.26c and ACTIVITY RISES from
4.49 to 6.09c (the chase lengthens with retune; this is the round-17
finding in the dial's own units). Activity PEAKS AT DIAL 40, the end of
the fast-and-full region, then falls monotonically through the taper:
6.09 -> 4.46 -> 2.34 -> ... -> 0.76c. A user sweeping the dial up will
hear it get busier to 40 and then progressively calmer and more
transparent. Above 50 every column tracks the Antares anchors within the
round-37 residuals (activity within 0.1c; off-grid +0.16..+0.43c; improve
+1..+3 points; word-start 0-1 vs 3-4). Below 40 there is no Antares
comparison: that is the deviation.

## Ruling 2 - THE UNMATCHED POSITION IS BOUNDED; filed, not pursued
Antares 0: ours 2.33c off-grid against their 1.55, 6.7 points fewer
within 5 cents, and nothing in the depth-by-speed grid closes it. The
round-4 onset-accuracy gap is now BOUNDED AT 0.78c rather than open-
ended. Recorded against trigger A of the rebuild ruling - the narrow-
band per-cycle detector (ONSET_SHAKINESS_RESEARCH.md section 4: Auto-
Tune's tracker searches +-N/2 lags around the current period with sub-
sample refinement; the ruling itself is cited from the round-38 ruling
text, not from a repo file of that name). Left unpursued: Sean's
priorities are the slow end and simplification, and 0.78c at the hardest
setting is not what he is asking about.

## Ruling 3 - the word-start result is recorded as a win in the arc (ONSET_PASS_RECORD.md), with its caveat.

## The compromised clip, recorded beside the approval (SLOW_END_RECORD round 32) - and the re-cut delivered
  /Users/SeanD/echojay-vst-pitch/earclips/depth_retune44__A_source__B_antares_retune50__C_echojay_retune44_depth50.wav   7,805,996 bytes
  Antares 50 by envelope (+32), at source level; for a fresh listen. The round-31 approval stands on the retune-150 clip.

NEXT (as ruled): the inventory ruling, then layout. No UI code yet. The
transfer function above is what the re-mapped dial will implement when
the inventory is ruled; DEPTH stays FRONT until that change (round 36
ruling 3), and moves to ADVANCED in it.

# Round 39 (5 Sep 2026): THE DIAL IS NOT MONOTONIC - a defect in ruling 1 that the transfer function exposed; a hard requirement; the fork waits on Sean's A/B

## THE DEFECT
From dial 0 to 40 activity RISES (4.49 -> 6.09c) and tuning LOOSENS (2.33
-> 4.26c) before the taper takes over. A user sweeping up hears it get
busier and less accurate for the first tenth of travel, then collapse.
That is §17.4's knee surfacing on the front panel, a consequence of
holding full depth across 0-40 (ruling 1 of round 38).

## HARD REQUIREMENT (ruled; a requirement, not a preference)
  THE DIAL MUST BE MONOTONIC IN ACTIVITY ACROSS ITS WHOLE LENGTH. TURN IT
  UP, IT DOES LESS. That is the contract a control owes its user and it
  is not negotiable against fit quality.

## THE CHEAP FACT THAT DECIDES THE SHAPE
Dial 0 (retune 6, full depth) DOMINATES retune 44 (Sean's working point,
the provisional default) on BOTH axes on this take: less activity (4.49
vs 6.09c) and better tuning (2.33 vs 4.26c off-grid; 69.7 vs 53.5% within
5c; improve 79.6 vs 61.4%; word-start events 0 both). He chose 44 before
the depth control existed and before several fixes landed, so it may be a
leftover rather than a preference. HE HAS BEEN ASKED to A/B retune 6
against retune 44 at full depth on his vocal. The clip is on disk:
  /Users/SeanD/echojay-vst-pitch/earclips/dial_fastend_AB__A_source__B_retune6_depth100__C_retune44_depth100.wav
  7,805,996 bytes, 3 legs, 0.45 s gaps, jointly normalised (both legs the shipped foundation, ign OFF, seam 60).

THE TWO BRANCHES, neither built, not averaged:
  - HE PREFERS 6: the 0-40 full-depth region collapses to a point; ruling
    1's premise goes with it; the dial is monotonic by construction. Re-
    map with a single fast-and-full anchor at 0 (6 ms, 1.0) and the fitted
    taper above: 50 -> (80, 0.35), 100 -> (150, 0.25), 200 -> (150, 0.15),
    400 -> (150, 0.10), linear between; the 0 -> 50 segment then carries
    both the speed change and the collapse.
  - HE PREFERS 44: keep a low region but make it monotonic by tapering
    depth across it so the activity rise from slowing is offset. THE
    RESIDUAL, from the existing grid (round 37) rather than a guess: at
    retune 44, depth 0.75 measures 4.61c activity and depth 0.5 measures
    3.07c; equality with dial 0's 4.49c falls at depth ~0.73, and strict
    monotonicity needs a shade below that. "Roughly 1.0 -> 0.85 by dial
    40" (the ruling's sketch) is NOT enough on this take - 0.85 lands
    near 5.1c, still above dial 0. So branch B costs his exact setting
    ~27% of its depth (1.0 -> ~0.73), taking its off-grid from 4.26c to
    ~5.1c at an unchanged improve-rate (~62%). Stated as the mismatch
    against his current setting; it is the price of a monotonic low
    region on this material.
  Do not build either until he answers.

## THE SHIPPED DEFAULT, FLAGGED
If retune 6 dominates retune 44 on this take, the provisional default
shipped in round 36 (retune 44 / flex 0 / humanize 0) may itself be
suboptimal. It was chosen because it is WHERE SEAN WORKS, not because it
MEASURED BEST - the round-36 commit relied on the former ("the measured-
safe neighbourhood, where Sean works and what he has ear-confirmed"). The
two are not the same justification, and the record now says which one
was used. Flagged for re-derivation alongside the broader material; the
candidate that measures best on this take is retune 6 at full depth.

The inventory ruling and layout still wait. No UI code.

# Round 40 (5 Sep 2026): BRANCH A - Sean's ear agrees with the measurement; ruling 1 WITHDRAWN; the curve verified; the default changes

## Sean's answer, verbatim, against the A/B (dial_fastend_AB clip, round 39)
"B is better on both" - retune 6 at full depth beats retune 44 by ear,
agreeing with the measurement (activity 4.49 vs 6.09c, off-grid 2.33 vs
4.26c). BRANCH A.

## Ruling 1 (round 38) is WITHDRAWN, not amended - recorded beside the original
Its premise was that Sean valued the fast-and-full point at 44; he does
not. The deliberate-deviation reasoning goes with it. The 0-40 full-depth
region collapses to a single fast anchor at dial 0.

## THE CONSEQUENCE, said plainly rather than presented as the plan all along
Antares' "missing middle" problem DISSOLVES. Our dial now matches their
shape at every anchor AND has a usable region between 0 and 50 where
theirs cliffs - not because we chose to deviate, but because a monotonic
curve through those knots produces one. That is a better outcome than the
deviation argued for in round 38.

## THE CURVE, verified by measurement - the linear interpolation FAILED, the shaped one PASSES
Knots (ruled): 0 -> (6 ms, 1.0); 50 -> (80, 0.35); 100 -> (150, 0.25);
200 -> (150, 0.15); 400 -> (150, 0.10). Above 50, linear in dial between
knots (verified monotone in every column, rounds 38 and 40). BELOW 50 the
interpolation shape is not free - it is what the requirement selects:
  v1 LINEAR in both (retune 6 -> 80, depth 1.0 -> 0.35, linear in dial):
     FAILS. Activity at dial 5 is 4.86c and at dial 10 4.64c against
     4.49c at dial 0; monotone only from dial 10. The chase grows faster
     between retune 6 and 25 ms than a linear depth fall offsets.
  v2 retune slow-start (6 + 74 t^2), depth linear:
     median monotone except one 0.03c uptick at dial 15 (4.09 vs 4.06 -
     within ruler noise, not strict); p90 rises 15.2 -> 18.1c and >25c
     3.4 -> 6.0% through dials 10-30. Not accepted.
  v3 retune slow-start (6 + 74 t^2), DEPTH LEADS (1 - 0.65 t^0.7):
     MEDIAN STRICTLY DECREASING at all 18 positions (4.49, 4.16, 3.87,
     3.66, 3.62, 3.47, 3.19, 3.12, 2.99, 2.81, 2.60, 2.34 | 2.12, 1.84,
     1.50, 1.13, 0.94, 0.76c). Tail residual, stated: p90 exceeds dial
     0's 15.59c by up to 0.58c at dials 25-35 (15.96 / 16.17 / 15.92) and
     the >25c share by up to 1.2 points (3.7 -> 4.9%). Every position's
     tails are below dial 40's of the withdrawn curve (30.2c / 13.3%).
  v4 depth leads harder (1 - 0.65 t^0.5):
     tails lower everywhere (p90 <= 14.7c) but the MEDIAN TIES at dials 25
     and 30 (2.86 = 2.86), >25c 3.8 > 3.7 at dial 30, and the first two
     dial units drop 14% of activity (an over-sensitive start). Not
     accepted.
  Full tables: tools/pitch_activity/logs_2026-09-03/transfer_tf2..tf5_2026-09-05.txt

## THE FINAL TRANSFER FUNCTION, proposed (v3)
    for dial d in [0, 50]:   t = d / 50
        retune_ms = 6 + 74 * t^2
        depth     = 1 - 0.65 * t^0.7
    for dial d in [50, 400]: linear in d between (50: 80, 0.35), (100: 150, 0.25), (200: 150, 0.15), (400: 150, 0.10)
Acceptance test (the round-39 requirement): median activity strictly
decreasing across the whole dial - PASSED at 18 positions on sourceNEW.
Tail residual as above. Antares anchors at 50/100/200/400 within the
round-37 residuals. The 0-50 shape (exponents 2 and 0.7) is a regime
constant (§17.5): re-verified on any material change and on the broader
takes. Not built.

## THE DEFAULT CHANGES: retune 6 / flex 0 / humanize 0 / depth 100 - provisional, with the hesitation on file
Round 36 chose retune 44 on "where Sean works"; both the measurement and
his ear now say retune 6 at full depth. Changed (kDefRetuneMs 44 -> 6;
schema text). THE HESITATION, recorded, not blocked on: retune 6 / full
depth maps to dial 0, the HARDEST setting, and defaulting a pitch
corrector to maximum correction is unusual - Antares ships at retune 20,
not 0. Our only material is one already-corrected, near-grid take, which
flatters hard correction. Shipped now because it is what the ear and the
numbers say; PROVISIONAL marker kept; "IS THE DEFAULT TOO HARD FOR RAW
MATERIAL?" is explicitly part of the broader-material re-derivation.
INSTALLED (suite 161/0; Logic running - relaunch before judging):
    AU   arm64 F73AEAF0-BB43-3B28-848C-485AF2530AD5
    VST3 arm64 185B5022-8BF4-34ED-B63D-B816AF6E700A

NEXT (as ruled): the inventory ruling, then layout. Still no UI code.

# Round 41 (5 Sep 2026): the constraint did the design work; THE INVENTORY PRINTED FOR SEAN

Recorded as ruled: the monotonicity requirement SELECTING the curve shape
(linear failed, depth-leading passed) is the right kind of outcome - a
constraint doing design work rather than a fit being tuned until it looks
acceptable. The residual (p90 up to 0.6c above dial 0 through dials
25-35, >25c share up to 1.2 points) stays visible in round 40 as
reported.

THE FULL CONTROL INVENTORY for Sean's ruling is CONTROL_INVENTORY_FOR_RULING.md
(on his Desktop and in the repo): every parameter, one per line, grouped
by placement with counts (13 front / 11 advanced / 5 internal), the
Antares equivalent or "no equivalent", close calls marked on the line,
and the six flagged items on their own lines (KEEP VIBRATO front as a
two-state switch with the Natural Vibrato name reserved; DEPTH front for
now, moving to advanced with the re-mapped dial; formant_mode proposed
INTERNAL; voice_type front per the ruling; tracking advanced pending
verification against Antares' eps; transpose unexposed pending its
octave defect). Layout follows his ruling on the set. No UI code.

# Round 42 (5 Sep 2026): "yes lets do it, remember that it needs to be dialabble" - the set settled; the vibrato gain moves up, scoped with its bar

## Interpretation, stated so it can be corrected
Proceed with both merges; the target is ANTARES PRO'S PANEL - real controls
the user turns - NOT the Access-style minimal panel where choices are made
automatically. "Dialable" reads as a rejection of "decided for you". If
Sean corrects this, re-rule.

## DESIGN PRINCIPLE (ruled): DIALABLE
  EVERY FRONT-PANEL CONTROL IS A CONTINUOUS DIAL OR AN EXPLICIT CHOICE.
  NOTHING IMPORTANT IS AUTOMATIC-ONLY. WHERE AN AUTO MODE EXISTS (KEY,
  REFERENCE), IT IS A STATE OF A CONTROL THE USER CAN ALWAYS OVERRIDE BY
  TURNING IT. It applies to the ADVANCED panel too: DEPTH stays a dial
  there; it does not vanish into the curve.

## THE MERGES (in the set; no UI code yet - CONTROL_INVENTORY_FOR_RULING.md v2, Desktop + repo)
  - key_source folds into the KEY control: auto is a BADGE on the dial;
    turning the dial takes it to manual. The value is never hidden by the
    mode. This also fixes the round-23 discoverability problem: the
    provenance state lived away from the value it governs, which is why
    the key source could not be read from the UI.
  - reference_source folds into the REFERENCE control the same way.
  - `correct` DOES NOT EARN ITS FRONT SLOT: the chain has a per-slot on/off
    (ChainHost::setSlotBypassed, latency-accounted), the device's own
    bypass runs the delay line at target 0, and round-35 verified that
    correct=0, mix=0 and bypass render the identical dry signal. It moves
    to ADVANCED as the model's master enable.
  Front: 13 -> 11 (merges) -> 10 when DEPTH moves with the re-mapped dial;
  `correct` leaving takes it to 9 at that point. 27 controls for 29
  parameters.

## THE NATURAL VIBRATO GAIN MOVES UP - it is the only front control that fails the dialable principle
Sean asked for Antares' +/-12 knob and ours is a switch because the gain
reaches audio only at natural_vibrato exactly 100. That blocks the panel
he asked for. Scoped here with its bar; NOT BUILT. THE CONVERGENCE (round
34) restated: the fast term must reach the audio at EVERY value, carried
on the co-timed ring - the phase-correct carrier the original corrector-
side attempt lacked (+7 clicks; (k-1)*osc hop-sampled and applied a third
of a vibrato cycle late).

SCOPING MEASUREMENTS (tools/pitch_activity PA_FASTRING, the engine's
existing ring-aligned fast term: a slow-pitch ring written co-timed with
f0, fastFactor = dev^(exponent) at the read pointer; corrector's hop term
removed; vibdepth survival ratio; the new vibrato-neutral NOTE-CENTRE
off-grid; word-start events; logs vibgain_A/B_2026-09-05.txt):
  Formulation A - SHIFT PATH for every k, exponent k-1:
    k     surv   activity   centre-off-grid   ws events   (retune 6)
    0     0.00    6.08c        7.18c            1          <- FAILS the hard end: shipped hard is 4.49c / 1.43c (the smoothed track cannot snap)
    0.5   0.72    4.30c        -                1
    1     0.97    4.16c        2.57c            1          <- the shipped keep within float rounding (max |diff| 2.2e-7; exact with a k=1 branch)
    1.5   1.27    7.71c        -                (10 ev)
    2     1.51   10.64c        5.55c            (11 ev)
  Formulation B - LEGACY PATH for every k, exponent k (k = 0 exact today's hard):
    0     0.00    4.49c        1.43c            0          <- BIT-IDENTICAL to shipped hard
    0.5   0.72    4.70c        2.31c            1
    1     1.04    6.14c        2.47c            4          <- FAILS word starts: the re-added wobble rides the onset excursions (shipped keep: 1)
    1.5   1.27    8.36c        4.51c            4
    2     (ruler saw no note)  10.16c   4.90c   4
  Read plainly: NEITHER PATH SERVES THE WHOLE RANGE. The shift path owns
  k >= 1 (k = 1 exact), the legacy path owns k = 0 (exact) and probably
  k < 1; the crossing at k = 1 is a path switch (PATH_UNIFICATION's
  question, now with numbers), and EXAGGERATION (k > 1) costs centre
  tuning 2.5 -> 4.5-5.5c and 4-11 events on both. A +/-12 knob whose top
  half degrades tuning is a product question for the ruling, separate
  from the DSP.

THE BAR (before any build; the ruled legs plus the two the scoping showed
are needed):
  1. THE GAIN APPLIES MONOTONICALLY across the range: vibdepth survival
     ratio strictly increasing over k in {0, 0.25, 0.5, 0.75, 1, 1.5, 2}
     on the synthetic vibrato note and on sourceNEW's sustains, with
     k = 0 -> <= 0.05 and k = 1 -> 0.95..1.05.
  2. BIT-IDENTITY AT THE VALUE THAT MEANS UNCHANGED: k = 1 renders
     bit-exact to today's natural_vibrato 100 (a k = 1 branch that skips
     the multiply; rounding-level differences do not pass). AND k = 0
     renders bit-exact to today's natural_vibrato 0 (the hard preset) -
     the scoping shows only the legacy path can give this.
  3. NO WORD-START REGRESSION: ign OFF NEW take, word-start events at every
     k in the sweep <= today's at the nearest shipped value (0 at k = 0,
     1 at k = 1); B's 4 at k = 1 fails this today.
  4. THE OLD-TAKE FALSIFIER unchanged at k = 0 and k = 1 (ign ON, tau 6).
  5. TUNING, vibrato-neutral: NOTE-CENTRE off-grid at k = 0 and k = 1 not
     worse than today's (1.43c / 2.57c at retune 6); at k = 2 the cost
     REPORTED, not gated - it is the product's call whether +12 ships.
  6. NO CLICKS: the LTP event count at k = 0 and k = 2 not above today's
     hard and keep (1 and 3 at retune 6) - the round-28 "+7 clicks at k
     = 0" not reproduced.
  7. SEAN'S EAR ON A SWEEP: one file, k = 0 / 0.5 / 1 / 1.5 / 2 as legs,
     his settings.
  Only after it passes does the control take the Natural Vibrato name and
  the +/-12 range (mapping proposed in round 33). Until then the switch
  stays and keeps its honest label.
  Design fork for the bar to decide (not pre-chosen): A for k >= 1 and B
  for k < 1 with a crossfade at 1; or the corrector's shift path made to
  snap (the PATH_UNIFICATION question); or dev^k on the legacy path with
  the onset excursions excluded (the seam-ramp / confirm-window state as
  the mask). Each is a measurement against legs 1-6.

Order: merges and the front-panel set (done here, in the set) -> the
vibrato gain with its bar -> layout. Still no layout code until the set
is settled. Engine note: setFastRing's upper clamp raised 2 -> 3 for the
scoping (production has fastRingOn_ off; no behaviour change).
