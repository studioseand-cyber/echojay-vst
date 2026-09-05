# EchoJay Pitch - the full control inventory, for Sean to rule on (5 Sep 2026)

Every parameter the device has, one per line: name | placement | what it does | the Antares equivalent.
Placement is a proposal. Argue with any line. Layout follows the ruling, not before.

## FRONT PANEL (11 now; 10 when DEPTH moves with the re-mapped dial)  - v2 after the merges (round 42)
key_root (+ key_source)   | FRONT | The Key control: root note (C..B). key_source is FOLDED IN as a badge on this control - AUTO shown on the dial when a bus is feeding it; TURNING THE DIAL overrides to manual. The value is never hidden by the mode. | Key
scale                     | FRONT | The scale (major, minor, chromatic, ...) whose notes are allowed.                                 | Scale
retune_speed_ms           | FRONT | THE dial, re-mapped 0-400: how fast AND how much it corrects, on the measured Antares curve.       | Retune Speed 0-400
flex                      | FRONT | How much expressive drift is left alone before correction engages (0 = correct everything).        | Flex-Tune
humanize                  | FRONT | Relaxes correction on held notes only, so long notes do not sound frozen.                          | Humanize
natural_vibrato           | FRONT | *** KEEP VIBRATO - a TWO-STATE switch: on keeps the singer's vibrato, off removes it. Labelled as the switch it is. The name "Natural Vibrato" is RESERVED for this slot until the continuous gain ships (DSP work, not built). *** | Natural Vibrato (theirs is a -12..+12 knob; ours is a switch today)
targeting_ignores_vibrato | FRONT | Stops wide vibrato confusing which note is being aimed at (a detector setting, not a sound one).    | Targeting Ignores Vibrato
reference_hz (+ reference_source) | FRONT | The Reference control (440). reference_source is FOLDED IN as a badge - AUTO shown when a bus feeds it; turning the dial overrides to manual. (Fixes the round-23 discoverability problem: provenance lived away from the value it governs.) | Detune
voice_type                | FRONT | *** Fits the detector to the voice (soprano / alto-tenor / low male / bass / instrument). FRONT per the ruling: getting it wrong costs accuracy and every probe once pinned the wrong one for a month. *** | Input Type
depth                     | FRONT | *** How much of the correction is applied (100 = full, 0 = the dry voice). The control that fixed the slow end and that you use daily. FRONT FOR NOW; it moves to ADVANCED in the same change that ships the re-mapped dial, because the dial will drive it. *** | no equivalent as a control (inside their dial)

## ADVANCED PANEL (12)
correct                   | ADVANCED | The device's own master enable. MOVED OFF THE FRONT: the chain already has a per-slot on/off and the device's bypass runs the same latency-preserving dry path (verified: correct=0, mix=0 and bypass render the identical dry signal). It stays for the model's dial and as a belt-and-braces switch. | (bypass)
seam_attack_ms            | ADVANCED | The word-start fix: the correction ramps in over this many ms when the wet path resumes after a consonant (default 60). A FIX, not a taste control. | no equivalent (prior art: GSnap Attack, Waves Note Transition)
tracking                  | ADVANCED | *** Detector strictness (relaxed / normal / tight): tighter drops more doubtful frames. ADVANCED pending verification that it maps onto Antares' Tracking (eps); it is our detector's parameter, not theirs. *** | Tracking (different semantics)
formant_shift             | ADVANCED | Shifts the vocal timbre up/down (throat size) - only audible in formant SHIFT mode, and that mode drops level 2.4 dB. CLOSE CALL: internal, with formant_mode. | Throat Length
low_latency               | ADVANCED | Lower latency at some cost to tracking. CLOSE CALL: Antares has it as a front mode switch.          | Low Latency mode
correction_mode           | ADVANCED | The character presets (natural / balanced / tuned / hard / custom); the default is now custom at retune 6 / flex 0 / humanize 0. CLOSE CALL: Antares ships presets. | presets
mix                       | ADVANCED | Wet/dry blend. The chain's wet knob already covers this. CLOSE CALL: Antares has Mix on the front.  | Mix
output_db                 | ADVANCED | Output trim.                                                                                        | no equivalent
vib_depth_cents           | ADVANCED | Generated vibrato: depth.                                                                            | Create Vibrato: Amount
vib_rate_hz               | ADVANCED | Generated vibrato: rate.                                                                             | Create Vibrato: Rate
vib_shape                 | ADVANCED | Generated vibrato: shape (sine / triangle / ramp).                                                   | Create Vibrato: Shape
vib_onset_ms              | ADVANCED | Generated vibrato: how long after a note starts it begins.                                           | Create Vibrato: Onset Delay

## INTERNAL / NO CONTROL (5)
formant_mode              | INTERNAL | *** PROPOSED INTERNAL, not advanced: off and preserve are the SAME SOUND for every correction under 2.5 semitones, which is all pitch-correction use. It only means something for octave shifts. A switch that does nothing does not earn a panel slot. *** | Formant (theirs is front; ours does nothing in practice)
transpose                 | INTERNAL | *** UNEXPOSED pending its defect: +12 gives a 155-cent shift instead of an octave in one region, -12 loses 3.7 dB (DEFECT_TRANSPOSE_OCTAVE). FRONT once fixed - Antares has it. *** | Transpose
ref_manual_by_user        | INTERNAL | A bookkeeping flag: was the reference typed by hand. Never a control.                               | no equivalent
target_hz                 | INTERNAL | An engineering diagnostic (fixed-target test path).                                                  | no equivalent
reset_stats               | INTERNAL | A momentary readout reset.                                                                            | no equivalent

Readouts (not parameters; live in the ADVANCED panel, always visible there): the detected-key line, the voice-fit suggestion, the reference provenance line.

TOTAL 29 parameters, 27 controls after the two merges: 11 front (10 when DEPTH moves), 12 advanced, 5 internal.

DESIGN PRINCIPLE (ruled): EVERY FRONT-PANEL CONTROL IS A CONTINUOUS DIAL OR AN EXPLICIT CHOICE. NOTHING IMPORTANT IS AUTOMATIC-ONLY. WHERE AN AUTO MODE EXISTS (KEY, REFERENCE), IT IS A STATE OF A CONTROL THE USER CAN ALWAYS OVERRIDE BY TURNING IT. "Dialable" - Sean's word. The one front control that fails it today is KEEP VIBRATO (a switch where Antares has a knob); the gain that makes it a knob is now the top DSP item, with its bar in UI_SIMPLIFICATION.md round 42.
