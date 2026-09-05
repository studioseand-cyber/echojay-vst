# DEFECT (general): A GUARD PREVENTS FUTURE CONTAMINATION. IT DOES NOT REMEDIATE VALUES ALREADY PERSISTED.

**Filed:** 2026-09-05, round-24 ruling. **THE ALARM THAT OPENED THIS FILE IS
RETRACTED (round 25):** "Sean's live session is applying 439.19" was wrong.
Loading his exact saved state through the real restore path measured the
APPLIED grid at 440.00 under auto (§4); the 439.19 is the dormant manual
FIELD, shown on the REF knob, armed only behind the manual switch. The
retraction stands next to the alarm because settling it by measurement
rather than by reasoning from the stored number was the correct move, and
the record should show both. The general defect below survives the
retraction intact. The instance: Sean's working project carries
reference_hz = 439.192 Hz in every save since 29 Aug - the circular auto-reference value (cf. his 29 Aug bounces at
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

## THE RULED REMEDY (round 25): ADOPT-ON-ENGAGE, not migration

**A MANUAL OVERRIDE, WHEN FIRST ENGAGED, ADOPTS THE VALUE CURRENTLY
APPLIED.** Flipping reference to manual yields the applied grid (440 for
Sean), not the stale field. Flipping key to manual yields the key in
force (root, scale - chromatic if auto had fallen back), not whatever auto
last wrote into the corrector. Adoption fires only at the auto -> manual
TRANSITION, from any path that makes it (the knob, applyStructured from
the model, a chain dial) - never on load, never while already manual.

MIGRATION (the earlier proposal: reset a non-user-typed auto-era field to
440 on load) CONSIDERED AND DECLINED, for the ruling's reasons:
  - No destructive reset, so no need to distinguish a user-typed 439.19
    from a laundered one - a distinction we may not be able to make (a
    pre-fix session has no ref_manual_by_user at all), and getting it
    wrong destroys deliberate settings.
  - One principle covers the whole class (reference, key root, scale, and
    any future auto/manual pair) instead of one migration per field.
  - A user already on manual keeps their value: adoption fires only at
    the transition.
  - The stale value never surfaces rather than being corrected after the
    fact.

WHAT ADOPT-ON-ENGAGE DOES NOT COVER, named:
  1. THE DISPLAY. While on auto the REF knob keeps showing the dormant
     field (439.2) beside a status line saying 440 - the exact thing that
     raised this file's alarm. Adoption never touches it until engaged.
     Companion rule, a display rule not a migration: while a source is
     AUTO, its manual knob shows the APPLIED value, greyed, and the field
     is neither shown nor editable until engaged (at which point it holds
     the adopted value). Filed here, not in the bar; a ruling.
  2. OTHER READERS OF THE PERSISTED FIELD. The saved reference_hz /
     key_root / scale stay at their stale or auto-derived values on disk.
     The engine never applies them without a transition, but a chain
     share, the dashboard, a forensic decode (this investigation), or the
     model reading "reference_hz": 439.19 will read the stale number AS
     THE SETTING. Adopt-on-engage protects the audio, not the record.
     Mitigation is the same display rule applied to serialisation: under
     auto, persist the manual field as the last USER-SET value (or the
     schema default), never the live auto-derived one. Filed; a ruling.
  3. A user ALREADY on manual with a laundered value and
     ref_manual_by_user = 1. Not constructible by any known path (the flag
     is set only by a user action on the control), so listed for
     completeness, not as a hole.
  4. NOT a gap but the reachability question the ruling asked: the trap
     is reachable WITHOUT THE USER TOUCHING THE CONTROL - applyStructured
     ({"reference_source": "manual"}) from the chat model or a chain dial
     applies manualRefHz_ today (setParamValue kRefSource, line 343). It
     still requires an explicit request to go manual, so it is not
     reachable by accident; adopt-on-engage covers it since it fires on
     the transition from any path. Sequencing stands unless a ruling
     weighs the model path as accidental.

THE BAR (rewritten; NOT built until committed here - this is the commit):
  1. Harness: load Sean's exact slot state (auto, field 439.19) - applied
     440, field 439.19 (unchanged on load); switch reference_source to
     manual -> applied 440.0 AND the field now reads 440.0 (adopted); a
     subsequent getStateInformation persists 440.0.
  2. Already-manual is untouched: a state with reference_source manual,
     ref_manual_by_user 1, reference_hz 439.19 loads and applies 439.19,
     and stays so through pumping (no adoption without a transition).
  3. The non-hand path: from Sean's state, applyStructured
     ({"reference_source": "manual"}) alone adopts 440, not 439.19.
  4. Key: under auto with an external F# minor applied, key_source ->
     manual keeps F# minor (adopts the key in force); under auto fallen
     back to chromatic (below gate), manual adopts chromatic; under a
     self-derived fact refused by the key guard, manual adopts chromatic,
     never the refused key.
  5. Renders on sourceNEW: Sean's state under auto renders bit-identical
     before and after the change; manual-after-transition renders
     bit-identical to a manual-440 render; a genuinely user-typed 439.19
     manual state renders bit-identical to today's build.
  6. Sean's project, after one load, manual engage, save on the shipped
     build: the decoded reference_hz reads 440.0.

## NEAR-MISS, filed as evidence of how the trap is reached

The advice given to Sean (round 22, section 14 of DEFECT_AUTOKEY_PROVENANCE):
"if his key or reference is not D minor at 440, have him set both
manually." FOLLOWING IT WOULD HAVE ENGAGED THE MANUAL REFERENCE AND
APPLIED 439.19. The control a user is told to reach for is the one that
arms the stale value - the trap is not reached by accident but by
following correct-sounding advice about the very control that exists to
fix the problem. He has been warned not to switch reference to manual
until adopt-on-engage lands; the round-22 advice is withdrawn as written
and replaced by: leave reference on auto (applied 440); set the KEY by
hand freely (it is already manual D minor).

## For Sean, now (before adopt-on-engage ships)
Leave reference_source on auto: his applied grid is 440 today. Do NOT
switch the reference to manual until adopt-on-engage is installed. (The
earlier suggestion to type 440 into the REF field by hand would also work
- it marks the field user-set - but it is one more manual action on the
control that arms the trap, and the ruled remedy makes it unnecessary.)

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

## Round 26: three rules folded into the adopt-on-engage bar

**RULE 1 - THE USER-SET FLAG (fixes the ruling's own hole).** Adopt-on-
engage as first stated destroys a genuinely typed value: someone who set
439.19 deliberately, switched to auto, and switches back would get 440.
ADOPT-ON-ENGAGE FIRES ONLY WHEN NO USER-SET FLAG IS PRESENT. New values
carry the flag (set by a user action on the control, never by state
restore, defaults, a mode write, or the model dialling a mode) and are
restored on re-engage; legacy values have no flag and are adopted over -
exactly the population to neutralise. The pattern already exists twice in
this codebase: ref_manual_by_user / refManualByUser_ for the reference
(EedPitchProcessor.h) and the chain pre-gain's userSet flag (ChainHost::isPreGainUserSet,
persisted as chainPreGainUserSet by PluginProcessor). KEY ROOT AND SCALE HAVE NO SUCH FLAG TODAY - one is
added (key_manual_by_user), same semantics, same persistence.

**RULE 2 - DISPLAY (approved, sharpened).** While a source is AUTO, its
manual knob shows THE APPLIED VALUE, GREYED - not the stored field. The
stored number is true of nothing under adopt-on-engage: not applied now,
not what engaging will apply. Dead data beside live data is the exact
configuration that produced the round-24 false alarm with the full record
in hand. Same test as rule 1: stored value shown when flagged (it is what
engaging will restore), applied value greyed when not. Same rule for key
root and scale.

**RULE 3 - SERIALISATION (approved; OUTRANKS the display rule).**
SERIALISED STATE IS SELF-DESCRIBING: IF A FIELD IS NOT IN EFFECT, THE FILE
NAMES WHAT IS. Write the APPLIED values alongside the stored ones -
overwrite neither: e.g. "reference_hz": 439.19 stays, and
"reference_hz_applied": 440.0 (plus "reference_source_applied": "auto")
is written beside it; likewise key_root_applied / scale_applied. The
readers are not hypothetical: a chain share carries the stale number into
another user's session, and the model reading the JSON is how the round-
24 alarm started. A reader must not need the engine's precedence rules to
know what the plugin is doing. Generalise to any future auto/manual pair.
The forensic decoder (pitch_key_forensic's state read) prints both.

**REACHABILITY ADDITION.** Setting a source to manual via applyStructured
WITHOUT supplying a value is an ENGAGE (adopt), never a silent adoption
of the stored field. Supplying a value with the switch sets the value and
the flag, as the knob would.

THE BAR, additional legs (7-11) on top of legs 1-6 above:
  7. FLAGGED VALUE SURVIVES AN AUTO ROUND-TRIP: type 439.19 on the REF
     knob (flag set), switch to auto (applied 440), switch back to manual
     -> applied 439.19, field 439.19, flag still set; persists through a
     save/load.
  8. UNFLAGGED LEGACY VALUE IS ADOPTED OVER: Sean's state (no flag,
     439.19) -> engage manual -> 440 (leg 1 restated under the flag test);
     a legacy state with reference_source manual and no flag loads as
     auto (the existing onStateApplied migration) and engaging adopts
     440.
  9. MODEL PATH: applyStructured ({"reference_source": "manual"}) with no
     value adopts the applied value; applyStructured ({"reference_source":
     "manual", "reference_hz": 441.0}) applies 441.0 and sets the flag;
     the same two shapes for key_source with/without key_root/scale.
 10. DISPLAY: with Sean's state on auto, the REF knob's displayed value
     is 440.0 greyed (not 439.19); after leg 7's typed value and a switch
     to auto, the knob shows 439.19 (flagged) greyed; key/scale knobs
     follow the same test.
 11. SERIALISATION: getStateInformation on Sean's loaded state contains
     both "reference_hz": 439.19 and "reference_hz_applied": 440.0 (and
     the key/scale applied fields); a state written before this change
     loads unchanged (the applied fields are read-only annotations, never
     consumed on restore); the mode test's schema/ledger walks are taught
     that the *_applied ids are annotations, not params.
Sequence: after the slow end and the timing lag, as ruled.

## Round 27 CHECK: does ref_manual_by_user mean what its name says? PARTLY - a finding, re-rule before building

1. SEAN'S STATE: ref_manual_by_user = 0.0 on 30 Aug and 3 Sep; ABSENT on
   29 Aug 16:42 (the field predates nothing - it did not exist before the
   29 Aug 17:28 fix). FALSE, and truthful: he never engaged the manual
   reference.

2. WHO WRITES IT (every site, EedPitchProcessor.cpp):
   - line 393, setParamValue(kReferenceHz), live (not defaults, not state
     apply): a VALUE write -> flips to manual AND sets the flag. The
     genuine "user typed it" path (or the model dialling a value).
   - line 345, setParamValue(kRefSource = manual), live: the SOURCE SWITCH
     ALONE, no value written -> sets the flag on WHATEVER THE FIELD HOLDS.
   - line 348, setParamValue(kRefManualByUser): state restore replays the
     persisted flag.
   - The auto-derivation path (refreshAutoKey) NEVER writes it, and never
     writes manualRefHz_ either: the 439.19 reached the field through the
     pre-fix state export, replayed on load with the flag absent/false.

THE FINDING: the flag does NOT discriminate value provenance. It means
"manual was engaged by a live action", not "the value was written by the
user". One gesture - flipping reference_source to manual without typing
anything, by hand or via applyStructured - stamps a laundered 439.19 as
user-set. That is "case 3, not constructible" from round 25, and it IS
constructible: every user who flipped the reference switch to manual
after 29 Aug 17:28 without typing a value carries flag TRUE with the
laundered field, is ALREADY MANUAL (so adopt-on-engage never fires - no
transition), and applies 439.19 today. Sean is not in that population.
Its size is unknown; its members show "ref 439.2 Hz (manual)" on the
status line, which is the only thing that surfaces it.

WHAT THIS DOES TO LEGS 7-11: they stand for VALUES WRITTEN GOING FORWARD
only if the flag is RE-SCOPED FIRST: set at line 393 (a value write), NOT
at line 345 (the source switch). Under adopt-on-engage the switch adopts
the applied value and does not flag it; typing flags it. That is a re-
rule, not an addition, because it changes what an existing persisted
flag is allowed to mean. Three options for the ruling, none built:
  (A) Re-scope the flag going forward and ACCEPT the legacy switch-only
      population as unreachable by adopt-on-engage; their mitigation is
      the visible "(manual)" status line plus the display rule. Cheapest;
      leaves a bounded, visible population on a 3.2c-flat grid.
  (B) Re-scope the flag AND, on load, treat a flagged manual reference
      whose value is not 440 as SUSPECT ONCE: keep applying it (no
      destructive reset), but show it loudly ("manual ref 439.2 - typed,
      or inherited? set it to confirm") until the user touches the knob
      (which re-flags it under the new meaning). Migration-shaped only
      in the readout; the value is never changed by the plugin.
  (C) Treat the legacy flag as meaningless (ignore persisted flags
      written before a version stamp; add ref_flag_version) and adopt on
      the first post-upgrade load for anyone manual-without-typed-value -
      this is the migration the round-25 ruling declined, and it cannot
      tell a typed 439.19 from a switched one either. Listed to show it
      was considered; not recommended.
Recommendation: (B). It keeps the round-25 principle (no destructive
reset), closes the visibility gap for the population (A) abandons, and
costs one readout string and one version-aware load check.

KEY ROOT / SCALE: no flag exists today, so nothing is mis-named there;
the new key_manual_by_user is DEFINED from the start as a value write
(key_root or scale written live), never the key_source switch.

The standing caution, restated for this instance: a flag existing is not
the same as the flag meaning what its name says - trace every writer.
