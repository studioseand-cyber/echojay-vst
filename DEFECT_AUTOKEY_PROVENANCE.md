# DEFECT: the auto-key path applies a key that nothing dates, nothing guards for self-derivation, and no bounce records

**Filed:** 2026-09-03, round-18 ruling C - reordered AHEAD of the timing
lag because it is the only mechanism that fails at TAKE scale, it is the
same family as two shipped defects (circular auto-reference, laundered
manual reference), and "a bounce inherits whatever was live" is a
measurement-integrity problem for the whole record.

## 1. Post-hoc recovery for the standing reference set (tools/pitch_key_forensic)

Nothing on disk records the applied key: the Logic project's plugin state
is opaque binary, dash-poll.log carries no key entries, the Link sidecars
none. Recovered from the AUDIO instead: sustained hops (>= 60 ms, < 4c/hop),
grid offset from A=440, tightness, pitch-class occupancy, best-fit keys.
Full log: tools/pitch_key_forensic/forensic_2026-09-03.txt.

    NEW take (alto_tenor)        reference    tightness   sustained content
    sourceNEW (dry)              439.68 Hz     4.65c       E 1.59s  F 2.88s  G 0.62s
    echojayignoreoffNEW 29 Aug   439.14 Hz     2.82c       E 1.65   F 2.78   G 0.58
    echojayignoreonNEW  29 Aug   438.99 Hz     2.65c       E 1.29   F 2.47   G 0.31
    echojaymaxretune     3 Sep   439.73 Hz     3.34c       E 1.28   F 2.74   G 0.41
    antaresNEW (@400)            439.62 Hz     3.90c       E 1.53   F 2.91   G 0.66
    antaresmaxretune             439.71 Hz     4.54c       E 1.43   F 2.90   G 0.55
    antares3 (hard)              439.91 Hz     1.36c       E 1.22   F 1.21   G 0.48

ROOT AND MODE - recoverable to a class, not to a key: the phrase's
sustained content is E, F, G only, so every applied scale that LACKS one
of them is excluded by their retention in every bounce (D major, E minor,
G major, G minor, Bb major - the damaging wrong keys, all excluded: a D
major bounce would have moved 2.8 s of F). The surviving candidates - D
minor, F major, C major, A minor, chromatic - give IDENTICAL targets on
this phrase (they differ on B/Bb, which the sustained material never
visits). VERDICT: no damaging key was applied to any NEW-take bounce; the
exact key cannot be recovered and did not matter for the sustained hops.
Residual risk: transitional hops touching B/Bb, unquantified, small.

REFERENCE - recovered, and it is a defect: the two STANDING EchoJay
reference bounces (29 Aug 16:42) sit at 439.14 / 438.99 Hz. The
circularity guard shipped in 1c5fb52 at 29 Aug 17:28 - 46 MINUTES AFTER
the bounces. They were made under the circular auto-reference defect,
tuned to the take's own centre: 2.1 / 2.7c flat of the source's grid and
3.4 / 4.0c flat of the 440 grid every off-grid number in this record was
measured against. Every tuning column quoting those two bounces (round 4
JOB 1's EJ_ignoreoff / EJ_ignoreon rows, REFERENCE_SET.md's "Sean's
vib-off bounce: 61.7%", the container's 29 Aug table) carries a ~3.4-4.0c
reference bias against Antares' 439.6-439.9. The in-process renders
(reference 440 by construction) and the 3 Sep max-retune bounce (439.73,
unresolvable from the source's own 439.68 at that retune) do not.

OLD take (low_male; the pitch_ab_test hard-match trio): NOT CONSISTENT.
dry.wav is 32% outside D minor (F# 0.42 s, best fit G major / E minor);
echojay.wav removed the F# (a scale without F#), echojay2/3 KEPT or grew
it (F# 0.78 / 0.22 s, best fit D major), echojay3's reference is 434.5 Hz
(-21.9c), antares.wav / antares2.wav fit E minor / D major. The record's
"the take's key is unrecorded, so its grid metrics are undefined" is
confirmed and sharpened: the old trio's members were made under DIFFERENT
applied keys and references and must never be compared for tuning.

RECORDED PLAINLY: the reference set carries a QUANTIFIED reference-
provenance defect (the two 29 Aug bounces, -3.4/-4.0c) and an
UNQUANTIFIED but bounded key-provenance risk (root/mode recoverable only
to an equivalence class that happens to be harmless on this phrase).
Matched-pair comparisons between in-process renders remain matched;
comparisons that quote the two 29 Aug bounces' absolute tuning are not.

## 2. Close the circularity gap: key root and mode from a self-derived fact

TODAY (EedPitchProcessor::refreshAutoKey): refCircular gates only the
TUNING REFERENCE. keyAuto applies f.root / f.minor from the same
self-derived fact unguarded. A vocal channel whose role is (mis)declared a
music bus keys the corrector off the singer's own melody; a flat or
modal singer defines the grid's root and mode.

PROPOSAL (not built): one guard, both quantities. A fact with selfDerived
&& publisherId == this instance is UNMEASURED for key as it already is for
reference: fall back to CHROMATIC (actively, via the existing
beginScaleCrossfade + applyScale(kScaleChromatic) path), never to the last
key; set a new AutoKeyState::keySelfIgnored for the readout, mirroring
refSelfIgnored; the readout line reads "key: chromatic (auto: only this
track measurable - not followed)".

THE BAR:
  1. tools/pitch_mode_test, new block beside the existing refCircular
     check: setKeyFeedSelfId(77), publish {root 6, minor, conf 0.86,
     selfDerived, publisherId 77}; scale reads "chromatic", all 12 degrees
     enabled, keySelfIgnored true, refApplied 440; the PREVIOUS external
     key does not survive; an external fact (publisherId != 77) restores
     key and reference; a self-derived fact from ANOTHER instance
     (publisherId != self) is still followed (a bus Link's self analysis
     is legitimate).
  2. In-process render on sourceNEW under a self-derived self fact is
     BIT-IDENTICAL to a manual-chromatic render; under an external bus
     fact it is bit-identical to today's build.
  3. Readout string present (grep-proof, LTO caveat noted: verify by
     UUID and behaviour, not by strings alone).

## 3. Make staleness visible (minimum) and the key path transport-aware

TODAY: the KeyFeed fact carries ageMs, but usable() ignores it for bus and
self sources, the pitch device's AutoKeyState drops it, and the [DETECTED
KEY] readout shows source, root/mode, confidence and reference - NOT AGE.
The KeyEngine needs 2 s of NEW audio per continuous pass; a stopped
transport freezes the key silently and indefinitely; nothing in the key
path reads the playhead; a bounce inherits the frozen value.

PROPOSAL (not built), two parts, visibility first:
  (a) VISIBILITY: carry ageMs into AutoKeyState; when age exceeds the
      engine's window (10 s default) the readout shows "held 37s" in amber
      beside the key, and "held since transport stop" when the outer
      processor's play-head reports stopped (PluginProcessor already reads
      getPlayHead()). Age alone NEVER changes the applied key - a paused
      song keeps its key; the chromatic fallback stays confidence-gated.
  (b) RE-VALIDATION AT PLAY START: on the stopped->playing edge, the key
      engine's hysteresis incumbent is released for the FIRST pass after
      the edge (one pass, then normal hysteresis), so a key that changed
      while stopped - a new song, a new take - is taken on the first 2 s of
      fresh audio instead of having to beat the incumbent's margin.
THE BAR:
  1. Harness: publish a fact, pump 15 s of silent blocks: state.ageMs
     grows, readout contains "held", the applied key is UNCHANGED; publish
     a fresh fact: "held" clears.
  2. Play-edge: stopped->playing with a different key in the next pass
     switches on that pass (today it needs the margin); stopped->playing
     with the SAME key produces a bit-identical render (no spurious
     crossfade).
  3. Sean's session, the day after install: the readout's age is READ and
     recorded in the next round - this project's pattern is that a
     readout catches the live problem within a day (the voice-fit readout
     did).

## 4. For Sean, today

With the take playing LIVE, read EchoJay Pitch's [DETECTED KEY] line. If
key_source is auto AND (the source says "this channel" OR the key is not
D minor / F major OR the reference is not 440.0), set the key MANUALLY to
D minor and the reference to 440 - the auto path can freeze a wrong key
across a transport stop and never shows how old it is. Then bounce again
and compare. That one readout is the whole live-vs-bounce check.

## 5. AUDIT of every surviving claim that rests on the two 29 Aug bounces (round-19 item 1)

Measured shift (tools/pitch_key_forensic, off-grid to D natural minor,
this ruler, onset = 150 ms after a word start; see
tools/pitch_key_forensic/offgrid_shift_2026-09-03.txt):

    bounce        grid     onset med  p75    p90    all-voiced med  <5c    improve-rate
    ign OFF     @ 440       6.50     10.25  16.88     5.11         48.9%    60.3%
                @ 439.14    4.79      9.08  16.96     3.76         59.2%    66.3%
    ign ON      @ 440       5.63     10.83  23.54     5.45         45.0%    57.1%
                @ 438.99    4.62     10.74  24.04     3.84         58.2%    64.7%
    antaresNEW  @ 440       6.34     13.30  27.24     5.41         46.9%    61.6%
                @ 439.62    6.41     12.82  27.24     5.39         47.6%    62.9%

The offset moves MEDIANS and the <5c share (medians -1.0 to -1.7c, <5c
+10 to +13 points, improve-rate +6 to +8 points) and leaves p90 and p75
essentially untouched (+-0.1 to -1.2c): a constant grid offset shifts the
centre of the off-grid distribution, not its tails. Claim by claim:

  - REFERENCE_SET.md "hard render 84/0.34s, 63.5%, 4.1c vs their 81/0.35s,
    61.7%, 4.4c": rough spans 81/0.35 SURVIVED (waveform metric, reference-
    free); improve-rate 61.7% SHIFTED -> ~67-68% (+6); off-grid 4.4c
    SHIFTED -> ~3.2c. The "matched settings" moral is unchanged.
  - REFERENCE_SET.md "his bounces measure 81/0.35 and 98/0.48": SURVIVED.
  - REFERENCE_SET.md "Sean's vib-off bounce: 61.7%": SHIFTED -> ~67%.
  - ONSET_PASS_RECORD round 4 JOB 1, the container's EJ rows (EJ_off
    6.03/10.38/21.78/44.7%; EJ_on 7.07/17.77/36.16/37.8%) and the round-4
    quoted medians EJ_off 5.85c / EJ_on 6.97c: VOID AS PRINTED as absolute
    rows against Antares - SHIFTED to approximately EJ_off med ~4.2-4.4c,
    <5c ~56-58%; EJ_on med ~5.9-6.0c, <5c ~50% (applying this ruler's
    measured deltas to the container's values; the container itself has
    not re-run). My own rows in that table: SHIFTED to 4.79/9.08/16.96/
    59.2% and 4.62/10.74/24.04/58.2%.
  - The onset p90 comparison EJ_off 21.78 vs ANT_0 9.51: SURVIVED - p90
    moves +0.08c on this ruler (16.88 -> 16.96); the ~2.3x ratio and the
    round-4 conclusion (Antares at Retune 0 corrects onsets ~2-3x tighter)
    stand. Stated, not assumed.
  - Round 4 jitter/contrast rows (onset jitter 7.07/7.18, sustain 0.97/
    1.61): SURVIVED (hop-to-hop variation, reference-free).
  - Round 4 JOB 2 "retired in favour of pf2_hard_v1_e0": CONFIRMED
    EXPLICITLY - the in-process rows (440 by construction, key fixed) were
    already the analysis instrument; the 29 Aug bounces were never the
    current engine. Now further superseded by the ref_2026-09-03 rows.
  - Round 4 cross-reference and DEFECT_VIBRATO_ON_TUNING_COST (lines
    "container onset p75 12.77 vs 18.71; here <5c 48.9 vs 45.0"): SHIFTED,
    ORDERING SURVIVES with a NARROWER margin - at the applied grids <5c
    59.2 vs 58.2 (gap 3.9 -> 1.0 points), onset p75 9.08 vs 10.74 (OFF
    still better). The filing's "not re-opened / consistent with the
    defect as it stood" status is unchanged.
  - Round 11 event rows (3 vs 11 events; 6.18s and 2.51s events in both
    bounces), DESIGN_SEAM_RESIDUAL's 6.18s citation, DEFECT_VIBRATO's "11
    vs 3": SURVIVED (LTP residual, reference-free).
  - Round 16 reconciliation "container had OFF better than ON": SURVIVED
    (decided on matched renders, not on the bounces).
  - PITCH_P0_VALIDATION's "the actual bounce 15.6c / 16.6%" table: NOT
    AFFECTED (a different, older bounce) - but it is an OLD-trio file,
    whose provenance section 1 voids for tuning anyway.
  - Nothing else in the record quotes these two files.

## 6. THE STALE-KEY HYPOTHESIS, TESTED (round-19 item 3; tools/pitch_keyhold_probe)

Production KeyEngine (continuous, defaults: window 10 s, sensitivity 50,
hysteresis 0.05) driven JUCE-free in 256-sample blocks with update() every
block; the production pitch chain (hard, ign OFF, tau 6, seam 60) rendered
under the held scale until the measured switch, then cross-faded to D
minor exactly as refreshAutoKey does. Logs beside the tool.

E1 CLEAN, sourceNEW from reset: the reading is C major at confidence
0.06-0.30 for the whole take - NEVER above the 0.50 gate. A solo vocal
does not key itself: the pitch device's own channel as auto source yields
CHROMATIC, always, on this take (the tuning estimate reads 436-438 Hz and
is gate-suppressed). E5: chromatic vs D minor on this singer differs on 2
of 2896 voiced hops (0.1%) - harmless here, which is also why the forensic
could not separate chromatic from D minor on the bounces.

E2 STALE from another solo take (dry.wav): incumbent below gate, first
post-play pass at 1.81 s, still below gate - chromatic throughout, no
damage. E3 STALE from the take's own tail: same.

E2' STALE FROM A CONFIDENT WRONG KEY (synthetic G-major progression, conf
0.75 - what a music bus or a Link on the wrong song holds): after 1 s of
stopped silence and play from the top, the G-major incumbent SURVIVES
4.0 s (the 2 s pass keeps it under hysteresis; the 4 s pass drops to C
major 0.35, below gate). DAMAGE (E4'): under G major until 4.0 s,

    0.0-2.5 s   off-grid-to-Dminor med 8.66c vs 2.87c clean; 240/504 hops
                (47.6%) a SEMITONE from the clean render
    2.5-5.0 s   4.77c vs 4.12c; 80/711 (11.3%)
    5.0-13.5 s  identical
    moved runs: E->F 0.26-0.33, 0.62-0.72, 0.81-0.86, 3.26-3.33 s;
                F#->F 0.74-0.79, 1.57-1.76, 2.94-2.99 s

CONFIRMED, WITH ITS PRECONDITION NAMED: a wrong key held across a transport
stop puts the first 4 s after play a semitone off on half the hops, then
heals - "off key for a tiny bit when I press play, not in the bounce" -
PROVIDED the key source is EXTERNAL (bus Link, channel Link, capture) and
its held reading is wrong for the take start. The device's own channel
cannot produce it on this material (never gates). Whether Sean's session
is in that configuration is the readout check in section 4.

WHAT THIS DOES TO THE STEP-3 BAR: the hysteresis release at the play edge
is not a design detail - it IS the fix for this defect - but it halves the
window, it does not close it: the incumbent survived the FIRST post-play
pass on hysteresis (2 s -> 4 s); releasing it takes the survival to the
pass cadence, 2 s of new audio. Bar amendment: (a) the first pass after a
play edge runs WITHOUT the incumbent's margin; (b) measured on this probe,
G-major survival after play <= 2.1 s (from 4.0), and the 0-2.5 s
semitone-moved share falls from 47.6% to <= 25%; (c) a same-key restart
(E3) stays bit-identical; (d) whether the post-edge first pass should also
run SHORT (1 s of new audio instead of 2) is a ruling, not a default -
listed as the lever that would take the window under 1 s, with its
false-switch risk stated.

## 7. RULING (round 20): the play-edge release goes to CHROMATIC; the bar is written against the ASYMMETRY

THE ASYMMETRY, which is the argument: the two failure modes point in
opposite directions and differ by roughly fifty to one. A stale WRONG key
actively moves notes to wrong pitches - 47.6% of the first 2.5 s a
semitone out (E->F, F->F#). CHROMATIC merely declines to snap to a scale;
on this phrase it differs from D minor on 2 hops in the whole take.
Section 4's principle restated: when the system is not sure, STOP
CORRECTING rather than correct confidently on a guess.

CAVEAT CARRIED: the 2-hop figure is a property of THIS phrase (E, F, G,
largely in-scale). On material where the singer sits off-scale often,
chromatic under-corrects noticeably for the length of the window. What
generalises is the asymmetry - a wrong key's damage is a semitone per
affected note, chromatic's is the singer's own off-scale drift on those
notes - not the number 2. The bar below is written against the
asymmetry.

CONSIDERED AND DECLINED - a shorter first pass after the play edge (1 s
of new audio instead of 2): it buys a smaller chromatic window (the cheap
thing) at the cost of deriving the key from less evidence (the expensive
thing). It optimises the side that does not hurt. The pass cadence stays
at 2 s of new audio.

DESIGN, superseding section 3(b) and section 6's bar amendment:
  - PLAY EDGE (stopped -> playing, from the outer processor's play head):
    the APPLIED scale goes to chromatic immediately (the existing
    beginScaleCrossfade + applyScale(kScaleChromatic) path - the same
    mechanism the confidence gate already uses) and stays chromatic until
    the FIRST fresh pass after the edge publishes a usable reading. The
    key engine's incumbent is NOT released to a challenger by hysteresis
    at the edge; it is simply not applied until re-confirmed. If the
    first fresh pass returns the same key, the cross-fade back is to the
    same scale: 2 s of chromatic, nothing else.
  - The readout says why: "chromatic (key held 37s - awaiting fresh pass)".

THE BAR (replaces the earlier step-3 bar; NOT built until committed here):
  1. tools/pitch_keyhold_probe, the forced-G-major arm (the positive
     control that must fire - section 8): after the play edge the APPLIED
     scale is chromatic from the first block; the G-major reading is not
     applied at any time after the edge; the first usable post-edge pass
     (>= 2 s of new audio) is applied when it arrives. Measured: the
     0-2.5 s semitone-moved share vs the clean D-minor render falls from
     47.6% to the chromatic-vs-D-minor floor of THIS material (0.1% here;
     recorded as "the chromatic floor", not as 0.1%).
  2. Same-key restart (E3 arm): the render after the edge equals the clean
     render everywhere except inside the chromatic window, where it
     equals a chromatic render; no third behaviour, no spurious cross-fade
     beyond the two (to chromatic at the edge, back at the pass).
  3. The off-scale-material arm (the caveat made measurable): a synthetic
     take with 30% of notes deliberately 40c off-scale - under-correction
     inside the window is bounded by the window length and the singer's
     own error, never a semitone; recorded as the accepted cost.
  4. Harness (JUCE processor, tools/pitch_mode_test): play-edge with an
     external usable fact -> scale reads chromatic; after >= 2 s of
     non-silent blocks and a fresh publish -> the key is applied; the
     readout string contains "held" and "awaiting fresh pass" inside the
     window and neither after.
  5. Sean's ear, on a real press-play at his session (once section 9's
     hold is resolved): the first 2 s must not be heard as off-key.

## 8. THE SAME LOGIC EXTENDED: every low-confidence or stale key state prefers chromatic over a held key

States that qualify, and what today's code does in each:
  - CONFIDENCE BELOW GATE: already chromatic (never the last key). Keep.
  - PLAY EDGE: section 7. Chromatic until the first fresh pass.
  - SOURCE SILENT / LONG GAP (the key engine hears no signal for longer
    than its window, 10 s default - transport stopped, or a source track
    that has gone quiet): today the reading is KEPT and applied
    indefinitely with full confidence. Under the asymmetry: a key whose
    evidence is older than the window is a guess; apply CHROMATIC, show
    "chromatic (key held 37s)", and let the next fresh pass restore it.
  - SOURCE LOSS (primary gone: a Link unloaded, a pinned source missing,
    the publisher's timer stopped): today, if the fact turns invalid the
    gate already gives chromatic; if the publisher keeps republishing its
    last resolved primary, the key is held with its age growing. Same
    rule: age beyond the window -> chromatic, with the reason shown.
  - SELF-DERIVED (section 2): chromatic, never followed.
  - GATE HOVER (confidence oscillating around 0.50 on a marginal source):
    chromatic <-> key toggles with a cross-fade every pass. Not damage in
    the semitone sense, but audible churn; a gate with hysteresis (enter
    at 0.50, leave at 0.40) is the obvious shape. Listed, not ruled.
  - The one state that must NOT go chromatic on age: a key set MANUALLY.
    Manual is not evidence-based and has no age.

CONSISTENCY WITH SECTION 3's "held Ns readout + never change the applied
key on age alone": half-consistent, and the half that conflicts is
resolved in favour of the asymmetry. "Never change on age alone" was
written to stop the device flipping to a NEW key on a clock; that stays.
Flipping to CHROMATIC on age is the safe direction - it declines to
correct - and it is what the asymmetry demands: if a key is too stale to
trust, chromatic is the safer application even without a transport edge.
Amended rule: AGE ALONE NEVER SELECTS A KEY; AGE BEYOND THE WINDOW
DESELECTS ONE. The readout remains the visible half of it - "held Ns" is
the reason string, no longer a warning about a key still in force. A
paused song resumes through a play edge anyway, so the two rules meet:
2 s of chromatic on every resume, then the confirmed key.

## 9. HOLD: the mechanism reproduces the symptom; it is NOT YET Sean's established cause

Section 6 established that a confident wrong key held across a stop
reproduces "off key for a tiny bit when I press play, not in the bounce"
exactly - PROVIDED the key source is EXTERNAL (bus Link, channel Link,
capture). On a solo take the device's own channel never reaches the gate
and stays chromatic, which cannot produce the symptom. So this is a
mechanism that reproduces the symptom, not the established cause of his.
THE PRESS-PLAY DEFECT STAYS OPEN until Sean reports what his [DETECTED
KEY] line says and what it names as the source. He has been asked
(section 4).

THE BRANCH, recorded so it is not quietly forgotten - IF THE LINE SAYS
"THIS CHANNEL" (or the key is manual): this mechanism is NOT what he is
hearing, and the other candidates return, in this order:
  (a) the corrector's mid-note resume at play - measured (SLOW_END_RECORD
      round 17): one note, ~40c step healed within 40 ms at tau 6, an
      altered first 500 ms at tau 150. Bounded, but real, and at his fast
      setting it is a 40c event on the first note of every restart.
  (b) the tuning-reference path: the self-derived tuning estimate reads
      436-438 Hz on this take (E1) - gate-suppressed today, but any state
      that lets it through (a mis-declared bus role; the pre-guard binary)
      is a -8 to -16c grid shift, take-wide.
  (c) capture -> bus precedence flip at 15 minutes (kCaptureKeyFreshMs): a
      key change at a wall-clock moment, live-only, with a cross-fade.
  (d) gate hover on a marginal external source (section 8).
  (e) Logic-side behaviour the offline chain cannot see: whether Logic
      feeds silence while stopped (decides whether the corrector's 200 ms
      note rule and the gate's 30 ms forget ever fire across a stop), and
      whether the plugin is processed at a different buffer size on the
      first block after play (block-size independence is by design; the
      grid-phase realisation difference is a null for "worse").
  (f) NOT a candidate: the 128-sample analysis-grid phase (established
      null: distributions identical, event lists merely re-realised).
If the line names an external source and a key that is not D minor / F
major, section 6 is the cause and section 7 is the fix.

## 10. DESIGN NOTE (round 21): the readout inversion - same string, opposite meaning

Under the pre-round-20 policy the proposed "held 37s" readout WARNED: a key
is still in force and its evidence is 37 s old. Under chromatic-on-stale
the same string EXPLAINS: a key is NOT in force, because its evidence is
37 s old. Same words, opposite meaning - and the second is the honest one.
A warning the user cannot act on (what would they do about a held key they
cannot refresh?) is worse than a label that says what happened and why
correction has gone chromatic. The readout is therefore specified as the
REASON STRING beside the applied scale ("chromatic (key held 37s - awaiting
fresh pass)"), never as a caution beside a key.

The point that generalises: WHEN A POLICY INVERTS, CHECK WHETHER ITS
READOUTS STILL MEAN WHAT THEY SAY. A readout is a sentence about state; a
policy change rewrites the state's meaning underneath the same sentence.
Every readout touched by a policy change is re-read, in the new policy,
for what a user would conclude from it.

## 11. FILED, NOT BUILT (round 21, low priority): manual-key DISAGREEMENT readout

Manual keys are correctly exempt from age (section 8) - they are not
evidence and have no clock. But exempt from age is not exempt from being
WRONG: a user can set D minor on a song that is not in D minor, and today
nothing validates it - no staleness, no disagreement, nothing on screen.

PROPOSAL: when key_source is manual and the auto path (the same KeyFeed
walk, running anyway) holds a USABLE reading that STRONGLY disagrees with
the manual key - a different root that is not the relative major/minor,
at confidence comfortably above the gate (>= 0.65, say), sustained for
more than one pass - the key line shows it, amber, greyed as a reading:
"key D minor (manual)   auto would say: G major 0.78 from \"Music Bus\"".
No behaviour change; the manual key stays applied. Relative major/minor
and enharmonic equivalents never trigger it (same degrees). Below the
confidence bar it is silent - a weak auto reading is not grounds to
second-guess a human.

WHY FILE IT: the make-hidden-state-visible pattern caught a live user
problem within a day of shipping the voice-fit warning (alto_tenor on a
low-male take), and the same pattern is what this whole defect file is
made of. Low priority because a wrong manual key is a user decision with
a visible control, not hidden state - but a readout that says "the song
disagrees with you" costs nothing at runtime and would have made the
OLD-trio provenance mess (dry.wav not in D minor, echojay3 at 434.5 Hz)
visible at the time instead of a week later by forensic. Left here.

## 12. BUILT BEHIND THE FLAG AND MEASURED (round 21): the key-side circularity guard

CODE (flag default OFF; flips only by ruling):
  - EedPitchProcessor: `debugKeySelfGuard(bool)` / `keySelfGuard_`;
    refreshAutoKey computes selfFact (selfDerived && publisherId == this
    instance) once and uses it for BOTH guards; with the flag on,
    keyUsable = usable && !selfFact drives the key block, so a self-derived
    fact takes the existing !usable path: beginScaleCrossfade +
    applyScale(chromatic), never the last key. AutoKeyState gains
    keySelfIgnored (set only when a USABLE fact was refused on
    circularity grounds).
  - EedPitchEditor: fallback line reads "auto: only this track measurable -
    key not followed - using CHROMATIC" when keySelfIgnored.
  - tools/pitch_mode_test: two new blocks (the bar's items 1 and 2), run
    log at tools/pitch_mode_test/run_2026-09-03.txt.

MEASURED (EchoJayPitchModeTest, build-release, commit of this file):
  bar 1, harness:
    baseline external fact followed (F# minor)                         PASS
    flag OFF: the self-derived key IS followed (the defect, documented)  PASS
    flag OFF: the reference guard already ignores it (440)              PASS
    flag ON: self-derived key NOT applied; keySelfIgnored set; scale
      reads chromatic; reference 440; previous key did not survive      PASS x5
    self-derived fact from ANOTHER instance (publisher 78) followed     PASS
    external fact restores key (F# minor) and reference (441.3)         PASS
  bar 2, render identity on sourceNEW (hard, 512-sample blocks, the
  JUCE processor end to end), self fact = G major 0.86 (F -> F#):
    guard ON + self-derived self fact == manual chromatic:  0 samples differ   PASS
    POSITIVE CONTROL guard OFF vs chromatic:          379,514 samples differ   PASS
    guard ON vs OFF under an EXTERNAL fact:                 0 samples differ   PASS
  bar 3, readout string: present in source; verified by behaviour
  (keySelfIgnored) not by strings (LTO caveat).
The positive control fired (section 8's method note): the run is valid.

FLAG STATE: OFF. The installed plugin is unchanged by this commit; the
flip to ON is the next ruling on this file.

PRE-EXISTING FAILURES SURFACED BY RUNNING THE SUITE (3, none from the
guard; all name seam_attack_ms, filed in ONSET_PASS_RECORD.md round 16):
  1. "every param constructs at its advertised default": a BARE
     EedPitchProcessor constructs seam_attack_ms at 0 (PsolaEngine's
     member initialiser) against the advertised 60. NOT a live defect:
     the registry writes schema defaults on creation
     (EedDeviceRegistry.cpp:87) and setStateInformation writes them
     before applying saved params (EedDeviceProcessor.cpp:229), so an
     added device and a restored session both run at 60 - the round-16
     ear gate and the installed default stand. It IS the hygiene rule
     the suite enforces (member initialisers match the schema; the pitch
     device's constructor, unlike the other devices', does not call
     resetParamsToDefaults). One-line fix, not made here.
  2. seam_attack_ms is absent from applyMode's correction_mode table and
     not exempted.
  3. seam_attack_ms has no hand control in the editor and no ledger
     exemption.

## 13. RULING (round 22): the guard flips ON bundled with Sean's key-line report - one wait, not two

The two decisions share one datum, his [DETECTED KEY] line:
  - EXTERNAL source (bus Link, channel Link, capture): the flip is a
    literal no-op for him (section 12, third arm: 0 samples differ). Flip,
    install, nothing changes audibly. Report it as done.
  - "THIS CHANNEL": the flip changes his sound substantially - from
    key-snapping (off the singer's own melody) to chromatic. NOT to be
    sprung mid-investigation while he is judging word starts: tell him
    what will change, let him A/B it deliberately (guard off vs on, same
    take, same settings), and record which he prefers.
What to say when reporting the flip, verbatim in spirit: CHROMATIC-UNLESS-
TOLD IS THE INDUSTRY-NORMAL DEFAULT. Auto-Tune ships chromatic and expects
the user to pick a key. This is not a downgrade and is not to be described
as a limitation; the key line already tells him what happened and what to
do (set the key by hand, or give it a music-bus source), which is the
readout pattern working as intended.
Until the report: flag OFF in the tree, installed plugin unchanged.

ADDENDUM (round 22, after the one-default fix): suite 150 PASS / 0 FAIL
(run_2026-09-03_b.txt). The three seam_attack_ms failures cleared: every
mode now reports "seam_attack_ms 60" in its applied summary, the bare
constructor consults the schema, the coverage ledger names the param. THE
RULING'S PREDICTION CAME TRUE IN THE SAME RUN: the round-21 render arms
constructed bare processors, so they ran at seam 0 - the harness WAS the
third path. The identity results stand (0 samples differ, both arms); the
positive control moved from 379,514 to 378,675 differing samples because
it is now measured at seam 60, i.e. the shipped default. The round-21
numbers were a valid measurement of the guard on a build one fix short of
the default; they were not a measurement of the default.

## 14. WHEN SEAN'S REPORT ARRIVES - act on the branch, do not re-ask (round-22 standing plan)

Three items resolve on one datum: what his [DETECTED KEY] line names as the
SOURCE (and the key / reference it shows).

SOURCE EXTERNAL (bus Link, channel Link, capture):
  1. Flip debugKeySelfGuard's default to ON (keySelfGuard_ { true }), build
     the four plugin targets at -j 4, install via tools/install_local.sh
     (~/Library only), dwarfdump --uuid, report the UUID. Tell him the flip
     is a measured no-op for his configuration (section 12, third arm).
  2. The stale-key mechanism (section 6) is CONFIRMED as the cause of the
     press-play defect. Close it against that cause.
  3. Proceed to the play-edge chromatic release against its committed bar
     (section 7).

SOURCE "THIS CHANNEL":
  1. Do NOT install the flip silently. Explain what changes: the plugin
     stops following a key derived from his own vocal and applies
     chromatic. Say plainly that chromatic-unless-told is the industry-
     normal default (Auto-Tune ships chromatic and expects the user to
     pick a key), not a limitation, and that the key line tells him what
     to set. Give him an A/B (guard off vs on, same take, same settings,
     one file per comparison per the deliverable discipline) and record
     which he prefers before anything ships.
  2. The stale-key mechanism is NOT his press-play cause. Re-open section
     9's branch with its candidates in the written order: (a) the
     corrector's one-note mid-note resume at play; (b) the gate-suppressed
     436-438 Hz self tuning estimate; (c) the fifteen-minute capture-to-bus
     precedence flip; (d) gate hover; (e) the Logic-side behaviours the
     offline chain cannot see. (f) The grid-phase realisation stays an
     established null - do not re-chase it.

EITHER WAY: if his key or reference is not D minor at 440, have him set
both manually, and note whether that alone changes what he hears.

Then the timing lag with its cancellation clause (SLOW_END_RECORD.md),
unchanged. No speculative work while waiting.
