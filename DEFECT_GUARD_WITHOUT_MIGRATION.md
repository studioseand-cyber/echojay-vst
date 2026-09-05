# DEFECT (general): A GUARD PREVENTS FUTURE CONTAMINATION. IT DOES NOT REMEDIATE VALUES ALREADY PERSISTED.

**Filed:** 2026-09-05, round-24 ruling. The instance that exposed it: Sean's
working project carries reference_hz = 439.192 Hz in every save since
29 Aug - the circular auto-reference value (cf. his 29 Aug bounces at
439.14 / 438.99 Hz). The circularity guard shipped 29 Aug 17:28. Any
session saved before that carries the bad value forever, and the guard
never fires on it because nothing is being re-derived: the value is just
LOADED. Every user who ran a pre-guard build has this.

## What is applied vs what is displayed, from the code (measured confirmation in §4)

  - reference_hz (the REF knob) IS the manual FIELD, manualRefHz_ - never
    the live grid (getParamValue kReferenceHz, EedPitchProcessor.cpp:436).
  - Under reference_source AUTO the applied grid comes from refreshAutoKey
    every block: usable && !refCircular ? tuningHz : 440. On Sean's
    topology (one instance, FullMix role, no external source) that is 440
    - the self-derived fact is refused (refSelfIgnored) or absent.
  - The status line shows the APPLIED value: "ref 440.0 Hz (auto)" or
    "ref 440.0 Hz (auto: only this track measurable - not followed)". The
    REF knob beside it shows the FIELD: 439.2.
  So the editor displays BOTH numbers at once, and they disagree by
  design: the knob is a dormant manual value, the line is the live grid.
  Sean's "D minor at 440" is the status line read correctly - he did not
  assume it. NOT a display/apply mismatch for the applied grid; the
  settings readings taken by eye stand. BUT the dormant 439.19 becomes the
  APPLIED grid the moment reference_source is switched to manual
  (setParamValue kRefSource: `if (manual) correct_.setReferenceHz
  (manualRefHz_)`) - a 3.2c-flat grid armed behind a control he was told
  exists to give him a manual reference. That is the live hazard.

## The audit: everything guarded rather than migrated

  1. TUNING REFERENCE, the 29 Aug circularity guard: GUARDED, NOT MIGRATED.
     The laundered-reference migration (onStateApplied) reverts a non-user
     MANUAL to auto - it migrates the MODE - but leaves manualRefHz_ at
     the laundered value. Sean's 439.19 has survived seven saves through
     that migration. THIS FILE'S INSTANCE.
  2. KEY ROOT / SCALE under auto: getParamValue(kKeyRoot) returns the
     corrector's LIVE root and kScale the live scaleIndex_ - a session
     saved under key auto persists the auto-derived key in the manual
     fields (the reference's pre-manualRefHz_ shape, unfixed for key).
     On load with key_source auto it is re-derived (now guarded), so it
     is dormant; switching key_source to manual keeps whatever auto last
     left in the corrector - the laundering path, for key. UNGUARDED AND
     UNMIGRATED. Not Sean's case (his key is manual, set by him).
  3. RETUNE CAP (§17.7): MIGRATED - clamp on load with "150 (was 400)".
     The model to copy.
  4. seam_attack_ms default: MIGRATED by construction - state restore
     writes schema defaults before saved params (round 22).
  5. KEY-SIDE CIRCULARITY GUARD (round 21-23): guards derivation; the
     persisted-key hole is item 2 above. Same class, filed here.
  6. voice_type default (DEFECT_VOICE_TYPE_DEFAULT): a default, not a
     guard; nothing dormant.

## The proposal (NOT built; bar first)

MIGRATE, ON LOAD, WHAT THE GUARD WOULD HAVE REFUSED:
  (a) Reference: on state apply, if reference_source is AUTO and the
      loaded reference_hz field is not 440 and ref_manual_by_user is 0
      (nobody typed it), the field is a laundered auto value: RESET IT TO
      440 and surface it in the readout once ("ref field was 439.2 from
      an earlier auto reading - reset to 440"). A user-typed manual value
      (ref_manual_by_user 1) is never touched. Persist the reset so it
      happens once.
  (b) Key: on switching key_source auto -> manual, seed key_root/scale
      from the manual fields' LAST USER-SET values if any, else CHROMATIC
      - never from what auto last left in the corrector; and stop
      persisting live auto-derived root/scale as if they were the manual
      fields (persist the manual fields; under auto persist the last
      user-set ones).
  (c) The rule for every future guard: A GUARD SHIPS WITH ITS MIGRATION
      OR WITH A LOUD READOUT OF THE UNMIGRATED VALUE. A guard alone is a
      promise about the future that leaves the past armed.

THE BAR:
  1. Harness: load Sean's exact slot state (the JSON in the mode test);
     after apply, reference_hz FIELD reads 440.0, applied 440.0, the
     migration readout flag is set; a state with ref_manual_by_user 1 and
     reference_hz 439.19 loads UNCHANGED (a typed value survives).
  2. Switching reference_source to manual after the migration applies
     440, not 439.19 (the hazard is closed).
  3. Key: save under auto with a self-derived key applied (guard off in
     the harness), reload, switch key_source to manual: the applied scale
     is chromatic (or the last user-set key), never the auto-derived one.
  4. Renders on sourceNEW: with Sean's state loaded and reference auto,
     bit-identical before and after the migration (his applied grid is
     already 440); with reference manual, the post-migration render
     equals a manual-440 render bit for bit.
  5. Sean's project: after one load+save on the migrated build, the
     forensic-decoded reference_hz field reads 440.

## For Sean, now (before the migration ships)
Open the REF knob's field: it reads 439.2. Set it to 440.0 by hand once
(that marks it user-set and clears the dormant value), or leave
reference_source on auto and do not switch it to manual until the
migration is installed. Either way his applied grid today is 440.

## 4. MEASURED (EchoJayPitchModeTest, Sean's exact slot state loaded via setStateInformation; run_2026-09-05.txt, suite 158/0)

    loaded: key_source manual, reference_source auto,
            reference_hz FIELD 439.19, APPLIED 440.00,
            seam_attack_ms 60 (absent from state -> schema default),
            retune 44.21
    self-derived 439.19 fact from his own channel:  applied stays 440.00,
            refSelfIgnored set; his manual key untouched
    reference_source -> manual:                     APPLIED 439.19

The three claims of §1 hold end to end: the applied grid under auto is
440, the REF knob shows the dormant 439.19, and the dormant value becomes
the applied grid on the manual switch - the guard never fires on a loaded
value. The suite is the bar's harness leg for the migration when it is
built (leg 1 will flip the FIELD expectation from 439.19 to 440.0).
