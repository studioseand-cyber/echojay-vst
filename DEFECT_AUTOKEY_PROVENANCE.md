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
