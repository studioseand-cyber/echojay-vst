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
