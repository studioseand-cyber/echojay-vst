# DEFECT (round 56, 5 Sep 2026): WRONG NOTE AT NOTE TRANSITIONS - the mechanism, measured; the fix, proposed (not built)

Opened by Sean's ear on the "different bit" section; measured by the
reviewer. His hypothesis (Antares limits the number of notes it fires)
was REFUTED by measurement: note switches per 250 ms over the sketchy
windows are source 7.83, Antares 7.45, EchoJay 7.21. No note-count
limiter. The defect is WHICH note and WHEN.

## STEP 1 - the baseline, REPRODUCED to the reviewer's numbers
Materials: /Users/SeanD/echojay-vst-pitch/earclips/note_transition_2026-09-05/
(source / antares / echojay _different_bit.wav, the three EVENT clips, the
reviewer's rulers). Driver: ruler_run.py beside them (tracks with
ruler_vtrack - continuity-constrained YIN, DP over tau, lam 1.6, W 2048,
hop 128 - then the wrong-note instrument and the transition classifier).
The reviewer's front end had to be found: the MEAN of the two channels,
the default RMS gate, frames voiced in ALL THREE files, t = frame centre.
With that, exactly his table:
    | | wrong-note events >= 25 ms (sub-octave) | total | at transitions | median error vs the source's own note |
    |---|---|---|---|---|
    | source | - | - | - | 11.00c (reviewer 10.99) |
    | Antares | 2 | 67 ms | 2/2 | 4.82c (4.81) |
    | EchoJay bounce | 10 | 541 ms | 10/10 | 7.99c (7.99) |
Event list identical (0.581-0.656 -102c, 0.987, 1.077, 2.893, 2.939,
3.013, 5.512, 6.163, 7.288, 7.789). Left channel alone gives 613 ms / 1
Antares event; that is the discrepancy that was fixed before proceeding.
POSITIVE CONTROL (ruler_control.py): a +c / 60 ms excursion planted at 21
fully voiced sites of the source (local resample at 2^(c/1200), 2 ms edge
fades; sites chosen where +-270 ms are voiced in all three files - the
reviewer's own first run planted into a gap and reported zero):
    +100c: fires 19/21     +60c: 11/21     +45c: 9/21
(reviewer: 21/21, 19/21, 11/21 - his plant is sharper than a resampled
window that the continuity-constrained tracker partly smooths; the
instrument reaches planted excursions, which is what the control asks.)

## STEP 2 - the configuration that produced the bounce, found by measurement (the standing rule)
The shipped configuration rendered through pitch_activity (retune 6 /
seam 60 / flex 0 / humanize 0 / KEEP VIBRATO off / IGN VIB OFF) gives 2
events / 64 ms in either key - ANTARES LEVEL, not the bounce. A grid over
retune, path and IGN VIB (logs in the record) found the match:
    retune 6, seam 60, nat 0 (LEGACY path), IGN VIB ON, key D minor:
    10 events / 581 ms / median 7.99c - the bounce's list EVENT FOR EVENT.
    the same with IGN VIB OFF:            2 events /  64 ms
    KEEP VIBRATO on (shift path), any:    0-1 events (and the tuning is the round-50 story)
    retune 44 / IGN VIB OFF:              8 / 336 ms;  retune 150 depth 25: 3 / 117 ms
So Sean's session is: dial 0, KEEP VIBRATO off, IGN VIB ON (the schema
default, 1.0 - his 3 Sep file had it OFF, this session does not), and THE
LEGACY TARGET/F0 PATH (shiftPreferred() is false at natural_vibrato 0;
the trace prints LEGACY on every hop). Renders: echojay_render_*.wav and
the two grid renders beside the materials.

## The instrumented render (tools/pitch_activity, PA_TRACE; trace_dminor_ignON_*.txt beside the materials)
Per hop: detector f0/voiced, the F0JumpGate (lastGood, bigJump, gated
f0), the corrector's inC, noteRef, slow track (and its age), the SELECTED
cents and its degree, the provisional flag, pending (cents, confirm ms,
n), gap and stable clocks, cur, target, the applied correction, the path,
the note-change count. Cents in the C frame: F3 = -700, E3 = -800, G3 = -500.

### Event A, t 0.55-0.70 - "EchoJay locks to E3 while the source reaches F3"
    0.552-0.563  source G3 falling (-512..-530); noteRef -493 (G3), target G3
    0.565-0.584  unvoiced 21 ms (gap < 200 ms: note kept)
    0.589        in -692 (F3): |in-ref| 199 > 90 -> PENDING (-692); provisional -> select = in -> target F3; cur glides -572
    0.597-0.603  in -755..-779 (the source dips toward E3): select = in -> target -800 (E3)
    0.605        in -786: 94c from the pending's -692 -> PENDING RESTARTS at -786 (confirm clock back to 0)
    0.605-0.619  in -786..-777, confirm 2.7 .. 13.3 ms; target E3, cur -774 -> -797
    0.621        confirm 16 >= 15 -> NOTE CHANGE FIRES with in = -762.3 (the source is already climbing back)
                 noteRef := -762.3 (a mid-glide sample, 38c above E3), slow track RESEEDED at -762.3, pending cleared, nc 2
    0.624-0.656  no pending -> IGN VIB selects the SLOW track (140 ms pole from -762): -762, -760, -757 ... -750
                 -> degree E3 until the slow track crosses -750 at 0.659; TARGET E3; cur -784 .. -799.9 (retune 6 ms)
                 while the SOURCE is on F3 (-708 at 0.632, -694..-698 from 0.640): APPLIED -88 .. -106c
                 |in - noteRef| = 55..68c < 90 -> the note-change detector CANNOT re-arm
    0.659-0.669  slow crosses -750 -> target F3 -> cur glides back; applied -67 -> -1
THE DECISION THAT PRODUCED E3 AT 0.60: the provisional selection of a
source that really dipped to E3+15c for ~15 ms (in = -786; that part is
the source). THE DECISION THAT HELD E3 WHILE THE SOURCE SANG F3
(0.624-0.656, the wrong-note event): the note-change CONFIRM firing on a
mid-glide sample (-762) and writing it as the note reference AND the slow
track's seed; from there the IGN-VIB target came from a 140 ms slow track
seeded 38c above E3, rounding to E3 for 35 ms, and the 90c re-arm
threshold measured from that mid-glide reference (68c away from F3) kept
the pending from firing. Retune 6 ms applied the E3 target at full depth.

### Event B, t 4.40-4.55 - "a note change that fired and unfired"
    4.400-4.472  unvoiced 104 ms (< 200 ms: the note is KEPT: noteRef -710.3 (F3 - 10c), slow -711 age 133 ms)
    4.475-4.493  source resumes ON E3 (-780 .. -799); |in-ref| = 70..89c < 90 -> NO pending
                 -> select = slow track (-712 .. -722) -> degree F3 -> TARGET F3 for a source on E3: APPLIED +80 .. +99c
    4.496        in -800.5: 90.2c -> PENDING (-800.5); provisional -> select in -> target E3; cur glides -736 -> -783
    4.507        in -799.7: 89.4c < 90 -> the pending is CANCELLED (else branch) -> select = slow (-730) -> target F3 again
                 cur pulled back UP: -753 -> -704 over 4.507-4.523; APPLIED +46 -> +96c
    4.525        in -800.5: pending again; 4.541 confirm 16 -> note change to E3 (noteRef -809), correct from here
THE DECISION THAT PRODUCED THE F3 PULL AT 4.47: the gap resume (104 ms <
kGapIsNoteChangeMs 200) kept the pre-gap reference (-710) and the pre-gap
slow track; the source came back one semitone LOWER, 70-89c from that
reference - under the 90c note-change threshold - so no pending armed,
and IGN VIB selected the target from the stale slow track: F3 for an E3
source, applied +80..+99c. When the source touched 90.2c the pending
armed and the target went to E3; at 89.4c it CANCELLED and the target
went back to F3. The threshold flickered on a source sitting exactly one
semitone from a reference that was itself 10c off its note.

## THE MECHANISM, named
WITH IGN VIB ON (the shipped default), THE TARGET NOTE IS TAKEN FROM THE
140 ms SLOW TRACK WHENEVER NO NOTE CHANGE IS PENDING, AND THE NOTE-CHANGE
DETECTOR MEASURES ITS 90c THRESHOLD FROM A REFERENCE THAT IS A RAW SAMPLE
(the input at the confirm instant, or the pre-gap input) RATHER THAN THE
NOTE THAT WAS DECIDED. A confirm that lands mid-glide (A) or a resume onto
the neighbouring note (B) leaves the source a semitone from its note but
under 90c from the reference: nothing re-arms, the slow track keeps the
old note for ~40 ms, and retune 6 ms applies it at full depth. Antares
tracks the source through the same passages because it never commits a
mid-glide sample as the note.
kNoteConfirmMs (15 ms) is a PARTICIPANT, not the cause: in A it is the
clock that happened to expire on a mid-glide sample; in B it plays no
part (the pending never lived 15 ms until the end). Widening it would
lengthen A. By the standing rule it is re-derived after the fix, on this
file, since its 15 was set against the pre-seam-ramp mechanism.
IGN VIB OFF removes the events (2 / 64 ms) because the target follows
the input at every hop - at the cost the round-27 measurements recorded
(vibrato flipping targets). Turning IGN VIB off is not the fix; the
default is on for a reason.

## THE PROPOSED FIX (not built; for the ruling)
Two changes to the note decision, both in EedPitchCorrect::process, no
new constants, no thresholds widened:
  1. THE REFERENCE IS THE NOTE, NOT THE SAMPLE. At every note start and
     every confirmed note change, noteRefCents_ and the slow-track seed
     are the DECIDED DEGREE (nearestDegreeCents of the confirming
     sample), not the sample itself. In A the reference becomes E3
     (-800); the source at F3 (-694..-708) is then 92-106c away, the
     pending arms at 0.632 and - because a pending makes the selection
     provisional - the target follows the input to F3 AT ONCE; the lock
     cannot form. (The slow-seed half of this is experiment (c),
     seedExp_ 3, already in the file from 30 Aug; unmeasured on this
     material.)
  2. A GAP RESUME RE-DECIDES THE TARGET FROM THE FRESH AUDIO. For the
     first confirm window after any gap (even one under 200 ms), target
     selection is provisional (select = input), exactly as during a
     pending; the 200 ms rule keeps the ENVELOPE position and the note
     reference, as the round-26/17.6 corridor work established, but the
     TARGET is re-decided from what is sung. In B the first hop after the
     gap (-780) selects E3 immediately; no F3 pull, no flicker.
  Not changed: kNoteChangeCents 90 (the vibrato guard), kNoteConfirmMs
  15 (re-derived after, per the rule), kGapIsNoteChangeMs 200, the
  F0JumpGate (it fired on nothing in either window), retune, depth.
Predicted against the bar: B1 - both worked instances are structural, so
the E3/F3 family (8 of the 10 events, all transitions between -17 and
-16) should fall to Antares' 2-3; B2 - the median cannot rise: the fix
changes only which note is targeted in the first ~40 ms after a change or
a resume, never the depth or the speed; B3 - improve-rate reported with
the render; B4 - the word-start seam ramp and the dial curve are
untouched (the fix acts on target selection, not the envelope or the
shift); B5 - suite; B6 - the three clips at 0.58 / 6.16 / 7.29.
FIRST STEP AFTER THE RULING, before shipping code: both changes behind an
investigation flag in the header (the file's own pattern: seedExp_,
envExp_), rendered through pitch_activity on this file, the ruler run,
the six legs measured - then the flag becomes the code or the proposal is
withdrawn by the numbers.

## What was read from the trace and NOT assumed (the standing rule)
The measured configuration runs the LEGACY path (every traced hop says
so); the F0JumpGate never fired in either window (bigJump 0, gated =
detector); the detector f0 is clean through both events (no octave
error: the wrong note is a DECISION, not a tracking error).

## ROUND 58 (5 Sep 2026): the condition isolated; the tree the reviewer read; the natural_vibrato thread closed; the 14 August premise measured

### The tree, first - so the defaults are not re-litigated
The reviewer's line numbers (kIgnoreVib at :72, kNaturalVib 100 at :133,
kMode kNatural at :22, kDefRetuneMs 120 at :213) are the feat/pitch
WORKTREE at /Users/SeanD/echojay-vst-pitch - whose Source last changed at
96d87d6 and which does NOT contain the round-50 default change (git branch
--contains 3330a0f lists only integration/reasoning-plus-pitch). The
installed build (AU arm64 7F0618CD) is built from THIS branch, where:
    Source/EedPitchProcessor.cpp:24-25   kMode default kCustom   (round 35/40)
    Source/EedPitchProcessor.cpp:49      kRetune (the dial) default 0 = 6 ms / depth 100
    Source/EedPitchCorrect.h:261         kDefRetuneMs 6
    Source/EedPitchProcessor.cpp:201     kNaturalVib default 0    (round 50, commit 3330a0f)
    Source/EedPitchProcessor.cpp:128     kIgnoreVib default 1     (ON)
    Source/EedPitchProcessor.cpp:535-540 kPresets: ignoreVib true in all four modes (unchanged)
    Source/EedPitchCorrect.h:1083        natVib_ { 100.0f }  <- a SECOND literal; dead at runtime
                                         (resetParamsToDefaults() writes the schema's 0 at construction,
                                         EedPitchProcessor.cpp:817) but it breaks "exactly one default" on paper
So THE SHIPPED DEFAULT IS: custom, dial 0 (6 ms / depth 100), flex 0,
humanize 0, natural_vibrato 0 (LEGACY path), IGN VIB ON, seam 60. That is
the configuration that reproduced the bounce event for event. Sean's
"IGN VIB was off" maps, as the reviewer said, to KEEP VIBRATO (the
natural_vibrato knob): it is off by default; the IGN VIB button is on.
My round-56 label "the shipped configuration renders 2 events" was WRONG:
that render was spec self:6:60:0:... - IGN VIB OFF - not the shipped
default. The reviewer's objection stands and is answered by the ladder.

### STEP 2b - THE ONE-AT-A-TIME LADDER (tools/pitch_activity on source_different_bit.wav, D minor, depth 100 unless stated; the reviewer's ruler)
    | configuration | retune | flex | hum | natvib | IGN VIB | events | total | median vs note |
    |---|---|---|---|---|---|---|---|---|
    | A0 natural preset (the reviewer's premise) | 120 | 55 | 60 | 100 | on | 0 | 0 ms | 11.61c |
    | A1 retune -> 6 only | 6 | 55 | 60 | 100 | on | 1 | 51 ms | 12.57c |
    | A2 flex -> 0 only | 120 | 0 | 60 | 100 | on | 0 | 0 ms | 12.06c |
    | A3 humanize -> 0 only | 120 | 55 | 0 | 100 | on | 0 | 0 ms | 11.69c |
    | A4 natural_vibrato -> 0 only | 120 | 55 | 60 | 0 | on | 15 | 856 ms | 15.87c |
    | B1 retune 6 + flex 0 | 6 | 0 | 60 | 100 | on | 2 | 88 ms | 14.46c |
    | B2 retune 6 + flex 0 + hum 0 | 6 | 0 | 0 | 100 | on | 2 | 88 ms | 14.48c |
    | B3 = THE SHIPPED DEFAULT | 6 | 0 | 0 | 0 | on | 10 | 581 ms | 7.99c |
    | C1 shipped default, IGN VIB off | 6 | 0 | 0 | 0 | OFF | 2 | 64 ms | 6.88c |
    | D1 HEAD's natural mode (dial 78.6 = 120 ms / depth 29) | 120 | 55 | 60 | 100 | on | 0 | 0 ms | 11.05c |
    | D2 natural preset, IGN VIB off | 120 | 55 | 60 | 100 | OFF | 0 | 0 ms | 10.89c |
    | the bounce | | | | | | 10 | 541 ms | 7.99c |
    (source median 11.00c; the natural-preset medians, 11-12c, are at or
    above the source: on the shift path at depth 100 the note is barely
    corrected at all, which is round 50's finding on this take.)
THE DISCRIMINATOR: from the natural preset, NO single factor among retune,
flex, humanize moves it (0-1 events); natural_vibrato 100 -> 0 ALONE takes
it from 0 to 15 events / 856 ms - the switch from the SHIFT path to the
LEGACY path, where the target is applied hard enough to be wrong. From the
shipped default, IGN VIB on -> off ALONE takes it from 10 to 2. The
condition is the PAIR: LEGACY PATH (natural_vibrato != 100) AND IGN VIB
ON. Each alone is inert: the shift path with IGN VIB on shows 0-2 events
(A0-B2), the legacy path with IGN VIB off shows 2 (C1).
SEVERITY, explicitly: BOTH ARE THE SHIPPED DEFAULTS. A fresh insert on the
installed build is B3. Every user hits this out of the box; it is not a
path Sean chose. THIS OUTRANKS EVERYTHING ELSE OPEN. (On the stale
feat/pitch defaults - natural preset - it does not fire, which is why it
was never seen before round 50 moved the default onto the legacy path,
and why the reviewer's premise build would not show it.)

### The natural_vibrato thread - CLOSED: no lost fix; one second literal to remove; three measurements, three rulers
The record's "default corrected to 0" is the SCHEMA default on this
branch (3330a0f, round 50), not the hard-mode preset (which was always 0
in kPresets). It is present in the installed build. The reviewer's 100 is
the stale worktree. The corrector's member literal natVib_ { 100.0f }
(EedPitchCorrect.h:1083) is a second default on paper, overwritten at
construction; it should read 0 - a one-line change for the ruling, not
made here. The three figures are three different measurements:
  - 15.7c vs 13.0c (EedPitchProcessor.cpp:518, spec §12, August): MEAN
    DEVIATION, hard tune vs the untouched signal, when modes did not write
    natural_vibrato at all - the mode-table completeness finding;
  - 22c vs 9c (feat/pitch's schema text, commit e23e3fa, "natvib trap
    named"; reworded out in 480b259): NEAREST-NOTE cents, retune 0 with
    natural_vibrato 100 vs hard mode, an earlier take and ruler;
  - 8.71c vs 6.72c (round 50): pitch_key_forensic ALL-VOICED OFF-GRID vs
    D minor @ 440 on sourceNEW at this branch's defaults, natural_vibrato
    100 vs 0. The same qualitative finding measured three times on three
    rulers; none of them is the hard-mode preset.

### The 14 August premise, measured (the regression leg the fix must pass, with its positive control)
The spec (PITCH_CORRECTION_SPEC.md:99-101, 180-186) states the artefact
without numbers: "wide vibrato on a semitone boundary chatters between
two notes" without the slow-track smoothing. Measured now:
  On the real takes, IGN VIB off does NOT chatter more than on: note
  switches per 250 ms sourceNEW 3.65 (on) vs 3.74 (off), source 3.77;
  different bit 4.65 vs 5.35, source 5.47; and OFF has FEWER wrong-note
  events (sourceNEW 0 vs 6 / 315 ms; different bit 2 vs 10). The premise
  does not fire on either take - so a leg on this material would have
  no positive control and would not be evidence.
  Synthetic notes (synth_vibrato_probe.py beside the materials; 6 Hz
  vibrato, 3 s; target-degree switches counted from PA_TRACE with
  count_target_switches.py):
    | note | IGN VIB on | IGN VIB off | pendings armed (hops) |
    |---|---|---|---|
    | F3 centred, +-60c (THE 14 AUG SCENARIO) | 62 switches / 2.6 s | 62 | 186/975 both |
    | F3 + 50c (on the boundary), +-60c | 31 | 31 | 186/975 both |
    | F3 + 50c (on the boundary), +-25c | 30 | 31 | 0/975 both |
  IGN VIB ON GIVES NO PROTECTION AT ALL TODAY: on the centred wide vibrato
  the 90c note-change threshold, measured from a phase-dependent raw
  sample as the reference, arms a pending on every wide excursion, and a
  pending makes the selection PROVISIONAL (= the instantaneous input),
  which bypasses the slow track. On a note parked exactly on the boundary
  the slow track itself straddles it (its +-5c ripple crosses the degree
  line), with or without pendings - that case is ambiguous by
  construction and is reported, not claimed. The reviewer's irony is the
  finding: the 14 Aug cure and the transitions defect are ONE mechanism -
  the reference is a sample, not a note.
  THE REGRESSION LEG, defined: on the centred F3 +-60c note, after the
  fix, IGN VIB ON must give ~0 target switches; POSITIVE CONTROL: IGN VIB
  OFF must chatter at about twice the vibrato rate (~30-60 / 2.6 s). Today
  both read 62, so the fix must DELIVER the 14 Aug protection, not merely
  keep it. The design ruling's C1 (reference = the decided degree) is
  what makes the pending stop arming on a +-60c vibrato around a note (60
  < 90); C3's re-derivation of the threshold decides the margin.

### What was NOT done this round
No code. No default changed. The C1/C2/C3 build behind the flag waits on
the ruling, with the six legs measured at BOTH the shipped default and the
reviewer's premise configuration, events paired per instant, and the 14
August leg above added.

## ROUND 59 (6 Sep 2026): two corrections to the record, then the build

### Correction 1 - THE RULE: A CODE FACT IS A CLAIM UNTIL THE TREE IT CAME FROM IS SHOWN TO BE THE ONE THAT BUILT THE INSTALLED BINARY.
The reviewer's line numbers in round 58 came from the feat/pitch worktree
(stopped at 96d87d6), not from the branch that built the installed binary,
and the inference drawn from them ("IGN VIB cannot be the discriminator")
was wrong. From here every quoted line number carries its commit. The
lines in the round-58 section are commit dd03c90's tree unless marked.

### Correction 2 - "each factor alone is inert" was WRONG; the ladder itself refutes it.
From the natural preset, moving ONLY natural_vibrato 100 -> 0 goes from 0
events to 15 events / 856 ms. Corrected statement: retune, flex and
humanize are inert; GIVEN IGN VIB ON (every preset, and the default),
natural_vibrato != 100 is SUFFICIENT ON ITS OWN. Both conditions are
necessary; neither is individually harmless.

### The worst row is not the default row
retune 120 + natural_vibrato 0 + IGN VIB on: 15 events / 856 ms, worse
than the shipped default's 10 / 581. Slow retune is the configuration
nine rounds fitted to Antares. It is a first-class row of the fix matrix.

### THE FIX MATRIX (ruled) - every leg measured at each row
    | row | retune | flex | hum | natVib | IGN VIB | baseline |
    |---|---|---|---|---|---|---|
    | R1 shipped default | 6 | 0 | 0 | 0 | on | 10 ev / 581 ms |
    | R2 worst case | 120 | 55 | 60 | 0 | on | 15 ev / 856 ms |
    | R3 protection-off control (GUARD) | 6 | 0 | 0 | 0 | off | 2 ev / 64 ms |
    | R4 natural preset (GUARD) | 120 | 55 | 60 | 100 | on | 0 ev / 0 ms |
R3 and R4 are guard rows: a fix that improves R1/R2 while degrading R3 or
R4 is a swap, rejected. B1-B6 stand at every row; B2 (median vs the
source's own note must hold at or below its current value) is judged AT
EACH ROW independently. B7: the 14 August protection delivered (centred
F3 +-60c probe: IGN VIB on -> near zero switches; OFF must still chatter
at about twice the vibrato rate - the positive control that must fire).
B8: one default per parameter ENFORCED - the corrector's natVib_ literal
aligned with the schema, and a test that fails if any engine-backed
parameter's member initialiser disagrees with its schema default.
IGN VIB off is NOT the fix and is not shipped silently: permitted only as
a labelled fallback, only if the fix fails its bar, only as a recorded
decision put to the reviewer with the failing numbers.
