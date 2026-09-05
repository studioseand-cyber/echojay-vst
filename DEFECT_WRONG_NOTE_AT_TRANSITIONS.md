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
