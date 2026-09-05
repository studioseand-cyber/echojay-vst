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
