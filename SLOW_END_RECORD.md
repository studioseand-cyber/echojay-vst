# The slow end of the retune dial (3 Sep 2026) - measured, attributed, NOT built

Sean's report, confirmed by container measurement on his max-retune bounces
(antaresmaxretune.wav / echojaymaxretune.wav, 3 Sep 13:04, Desktop): at max
retune Antares is transparent and we are not. This file is the ruling's
three deliverables - the seam_attack self-check, the dial sweep, the
attribution - plus the transport-start inventory. No engine code changed.
Ruler: tools/pitch_activity (new; production chain in-process with the
effective-ratio tap; raw logs in tools/pitch_activity/logs_2026-09-03/).
Voice alto_tenor, D minor, 440, sourceNEW (636096 samples).

ACTIVITY = |output - source| in cents per 2.67ms hop, hops voiced in both.
Same ruler on Sean's bounces reproduces the container's table to within
ruler noise (ANT 0.57/1.31/2.49c >25c 1.0% vs their 0.61/1.35/2.97/1.5%;
EJ 5.83/13.85/31.59/12.7% vs their 4.90/12.66/30.44/12.4%).

## 0. Which settings Sean's bounce was at (established by event timeline)

Hard base (flex 0, humanize 0, natural-vib 0), retune 150, seam 60. The
ignore-vib OFF render's >25c event list matches the bounce hop-for-hop from
1.41s on (1.41:-180, 1.93:-206, 2.18:+32, 2.48:-85, 2.84:+76, 3.10:+72,
3.30:-61, 3.38:+88, 3.41:+37, 4.19:+142, 4.29:-88, 4.43:+82, 4.48:-27 ...)
once the analysis-grid phase is matched (section 4). Natural/balanced bases
are 5x calmer (median 1.0-1.4c) and cannot be his bounce.

## 1. FIRST JOB - is this our own fix? NO. seam_attack 60 is CALMER at tau 150

    tau 150, hard base          median   p75    p90   >25c%
    ign OFF  seam 0              7.46   21.22  48.87  21.9
    ign OFF  seam 60             6.25   15.98  37.56  16.0
    ign ON   seam 0              6.99   22.12  63.53  23.2
    ign ON   seam 60             6.07   15.80  46.85  17.3
    natural  seam 0              1.44    3.84  10.30   0.5
    natural  seam 60             1.06    3.13   8.29   0.3

Every column improves with the ramp on, at every base. The seam ramp has
no bad interaction with long retune; the slow-end problem PREDATES the fix
and is independent of it. The default stands (its own caveats unchanged).

## 2. The sweep, in the units Sean hears (hard base, seam 60)

    ign-vib OFF   median   p75    p90   >25c%  | unintended med/p90 | snap-window peak|d| med
    tau   6        3.95   8.19  14.50   3.8    |   6.96 / 29.2      |   28.5c
    tau  40        4.92  10.94  21.91   7.9    |   8.27 / 29.6      |   33.2c
    tau  80        5.76  13.69  31.13  13.4    |   9.50 / 40.6      |   48.5c
    tau 120        6.03  15.01  35.38  15.5    |   9.76 / 45.2      |   55.4c
    tau 150        6.25  15.98  37.56  16.0    |   9.88 / 47.4      |   55.6c
    ign-vib ON
    tau   6        4.03   8.77  18.35   6.9    |   6.91 / 29.8
    tau  40        5.30  13.54  36.06  14.4    |   9.40 / 42.1
    tau  80        5.68  14.62  43.36  15.8    |   9.86 / 49.7
    tau 120        5.91  15.43  46.28  16.6    |   9.80 / 51.9
    tau 150        6.07  15.80  46.85  17.3    |   9.81 / 54.2
    ANTARES max    0.57   1.31   2.49   1.0    (it is not correcting)

"Unintended" = |activity - commanded|, commanded being the engine's own
effective ratio at the read pointer (the effR tap), in cents. Unity/grain
control: 0.00c on every column (the ruler's floor at unity is exact).

THE FRAMING THAT MUST TRAVEL WITH THE 8x: Antares at its max retune applies
essentially no correction on these note lengths (activity 0.57c, 1.0% of
hops >25c - transparent because inactive; §17.4's "tau never arrives"). Our
max is 150ms, a setting that still corrects hard (commanded shift +18.5c
mean over the first phrase). So "8x at median" compares correcting with not
correcting. The DAMAGE column is the unintended one: it rises from ~7c
median / 29c p90 at tau 6 to ~10c / 47c at tau >= 80, and the >25c event
share rises 4x. That is the slow end's cost, and §17.4's "useful correction
ends ~40ms" reads the same in these units: the knee is between 40 and 80.

## 3. Attribution (ign-vib OFF, seam 60; share of total |activity|)

    tau    SNAP    SEAM   DETECT  GLIDE  | mean |d| per bucket (c)
      6    37.3    3.5    27.3    32.0   |  8.0   3.3   10.5   5.2
     40    29.1    9.3    29.0    32.6   | 12.8   6.2   13.0   7.2
     80    40.0    9.8    24.2    26.0   | 15.3   8.7   15.6   8.5
    120    40.2    9.7    23.3    26.7   | 16.7   9.7   16.9   9.7
    150    41.7    8.9    22.8    26.6   | 17.5   9.4   17.4  10.4

Buckets, priority order: SNAP = within 100ms after a >50c target jump (the
note-boundary chase + confirmation snap, envExp 0 since f1d9f5f); SEAM =
within 60ms after a >=15ms dry re-entry; DETECT = the gated f0 the shifter
believed for this audio differs from the fine source track by >10c;
GLIDE = the remainder, the envelope's own travel.

  - THE NOTE-BOUNDARY SNAP IS THE SLOW END. Its share grows 37 -> 42%, its
    per-hop mean 8 -> 17.5c, and the peak activity inside snap windows
    doubles, median 28.5c -> 55.6c, tau 6 -> 150 (the record's 4.8c -> 173c
    is the target-step size; this is what reaches the audio). It is the
    only bucket whose SIZE scales with tau; the others scale weakly.
  - DETECTOR ERROR PASSING THE RATIO is the second bucket (23-29%), and
    it is a TIMING error, not a pitch error: inside fast glides the f0 the
    shifter attaches to the audio leads the audio by ~30ms of glide (dump,
    3.20-3.27s: f0Here-src +24..+42c on a 100c/80ms move; 3.04-3.06s:
    -43..-66c on the move down). Steady notes agree to +-5c.
  - THE SEAM RAMP is 9% and REDUCES activity (section 1). Not a suspect.
  - Sean's 3s glitch, mechanism (dump 2.40-3.50s, tau 150 ign OFF): the
    E->F change at 3.20-3.30s. The envelope is still chasing the OLD
    note's aim (target-src -20..-36c) while the detector leads the glide
    (+24..+42c); commanded = target - f0Here reaches -71c, applied to
    audio that is already at the new pitch -> output 40-61c BELOW source
    for ~60ms until the confirmation snap (3.275s: -71 -> -15 -> +1c in
    two hops). Same shape at 3.04-3.09s in the other direction (+67c for
    30ms) and at 2.81s (a re-entry with f0Here a whole semitone stale:
    +107c belief, +77c output for one hop). Container's 2.60/51c and
    3.30/19c are these events on a coarser ruler.
  - Word start vs mid-note: at seam 60 our rows move slightly LESS at word
    starts (13.0 vs 14.3c; Antares 1.75 vs 1.89c) - the container's
    "opposite behaviour" does not reproduce on this ruler at seam 60. It
    DOES reproduce at seam 0 (22.2 vs 16.9c): that was the seam step, and
    the ramp removed it.

## 4. The transport-start defect: inventory and measurement

FIRST, a null that matters: Sean's bounce event list matched a stale-restart
render exactly (1.41:-180, 2.48:-85, 4.29:-88 appear; the clean render has
none of them and 4.29:+188). Ablating EVERY component's state at the
restart (corrector, gate, detector, shifter, held target) did NOT restore
the clean list. Pre-padding the input by 64 zero samples did reproduce the
"stale" list exactly; 1, 128 and 256 samples reproduce the clean one. The
source is 636096 samples = 64 mod 128: THE DIFFERENCE IS THE 128-SAMPLE
ANALYSIS-GRID PHASE, not carried state. The same audio 1.3ms later on the
hop grid turns a nothing at 1.41s into a -180c event and flips 4.29s from
+188 to -88. Distributions are stable across phase (median 6.23-6.27,
p90 36.0-37.9, >25c 15.6-16.1%); EVENT LISTS ARE REALISATION-DEPENDENT and
must not be compared hop-for-hop across bounces made at different offsets.
Sean's bounce is the 64-offset realisation. Container's octave findings at
0.14s/8.8s: excluded here as |d|>600c tracker disagreements (0 in ANT, 5 in
his EJ bounce, 1-9 per render); not chased, per ruling.

WHAT PERSISTS ACROSS A STOP/START (code, Source/EedPitchProcessor.cpp and
the three engine headers): nothing in the pitch device reads the playhead.
Every "reset" is a clock on the device's own gap counters, which advance
ONLY while processBlock is called:
  - corrector: haveNote_/curCents_/noteRefCents_/targetCents_/slowCents_
    survive until 200ms of UNVOICED HOPS (kGapIsNoteChangeMs); pending
    state survives (pendForget off).
  - F0JumpGate: lastGood_ survives until 30ms unvoiced (kGapForgetMs).
  - PitchEngine: median-of-3 history survives until 0.5s unvoiced
    (kHistoryClearS); hop/input position counters never reset.
  - shifter: curTarget_, spliceR_ (the slewed ratio), spliceDrift_,
    seamRampW_, the input ring - reset only by prepare()/reset(), which
    nothing calls on transport.
  - processor: lastTarget_/lastShift_/lastHopF0_/lastHopVoiced_ are held
    across blocks indefinitely ("hold the last target through the gap").
  - KeyEngine (auto key): accumulation persists across playback; has
    reset() but it is not transport-driven. NOT in the offline chain, NOT
    measured here - the one candidate that could make LIVE differ from a
    bounce over a whole take rather than one note, if auto-key is in use.
So: if Logic keeps calling processBlock with silence while stopped, the
clocks expire and a live start is clean after 0.5s of stop. If the first
block after play follows the last block before stop with no silence
between (a stop mid-phrase, or a bounce started straight after live play),
the corrector is mid-note and the next note is treated as a RESUME of the
old one.

MEASURED (tools/pitch_activity selfc: previous playback stopped mid-note,
then play from the top, no silence between; hard base, seam 60):
  - tau 6, stopped at 5.3s: first hops +16 +15 then -25 -23 -21 ... a ~40c
    step 8ms into the first note, healed by 40ms (the resumed old note's
    correction applied to the new note until the note-change confirms).
    Clean render: -4 -6 -27 -26 ... at seam 0 (the seam step), +1..+9 at
    seam 60.
  - tau 150, stopped at 2.0/5.3/8.0s: first-500ms trajectories differ from
    clean (0-250ms mean 18/42/21c vs 36c clean; 250-500ms 7/25/34c vs
    27c) - different, not uniformly worse; the resumed envelope glides
    from a stale position at tau's pace.
  - By 0.5s every restart variant coincides with a clean realisation.
    The transport-start effect is ONE NOTE, bounded at ~40c/40ms (tau 6)
    and ~500ms of altered glide (tau 150). It does not explain a whole
    take sounding worse live.
  - Sean's bounce's own first 130ms (-55c rising through 0 to +10c) is
    neither the clean nor the phase-shifted clean first note (+1 -> +46c):
    it was rendered from carried state - consistent with Logic starting
    the bounce straight after live play. Its first note is the transport
    defect; its 1.41s-onward list is phase.

The first-250ms "13c decaying over 750ms" in the container's table is the
correction of the first phrase, not a transient: clean render 0-250ms mean
36c with commanded +18.5c vs measured +20.9c signed; Antares applies none.

## 5. What this record does NOT do

No bar, no fix. Candidates are already on file: the note-boundary release
(envExp 5, reverted f1d9f5f, re-enable only through its four-part
acceptance) attacks the SNAP bucket directly; the detector's glide lead is
a timing alignment question (pitchLag / f0At at the read pointer) that has
never been measured as such. Both need their own bar. For Sean, now: keep
retune LOW on this material (tau 6-40 is the calm end of every column
above); the slow end is the broken end.

# Round 17 CORRECTION (3 Sep 2026, ruling 2 diagnosis) - the "detector lead" was the RULER

tools/pitch_lead_probe (synthetic 20-harmonic voice, analytic f0(t), 200c
linear-in-cents glides at 0.25/0.5/1/2/4 c/ms, both directions, no gaps)
measures three observers against truth at the position each claims to
describe. Lead fitted as the time shift minimising |cents error| over the
glide's central 60%. NEGATIVE = the observer LAGS the audio.

    alto_tenor, 165 Hz         HOP event (raw)   RULER (8192-blk)   SHIFTER f0Here@read
    0.25 c/ms                     -30.6 ms          -27.9 ms            -6.6 ms
    0.5                           -30.7             -28.0               -6.6
    1.0                           -30.5             -27.8               -6.5
    2.0                           -30.2             -27.7               -6.4
    4.0                           -29.4             -26.8               -6.7
    low_male, 110 Hz              -41.9             -39.3              -10.1
    steady-note |err| at the shifter: 0.04c (alto), 0.13c (low)

THE DIRECTION IS LAG, NOT LEAD, AND THE MAGNITUDE IS 6.5 ms, NOT 30. The
"src" track pitch_activity used as truth is the same estimator at 8192-
sample blocks and LAGS THE AUDIO BY 27.9 ms (alto). Comparing the shifter's
f0 (lag 6.5 ms) against it reads as a 21 ms LEAD - exactly the "24-42c on a
1.25 c/ms glide" filed in section 3. RETRACTED: the DETECT bucket's
direction and size, the "unintended 10c/47c" column (it was the same
misalignment applied to the commanded ratio), and the detector-lead half
of the 3s account. Sections 2-3 above stand for the ACTIVITY columns
(both files carry the same ruler lag; the difference is unaffected) and
for the seam self-check; their attribution and unintended columns are
superseded by the aligned table below.

## Aligned attribution (ruler lag 27.9 ms applied to every tap lookup; hard, ign OFF, seam 60)

    tau   SNAP share/mean   SEAM        DETECT share / hops / mean / >25c   GLIDE share/mean   UNINTENDED med/p90   snap-window peak med
      6    33.7%   6.8c    4.7%  3.4c   14.8%   6.4%   16.1c   38 of 111   46.8%   6.5c        1.20 /  7.1c          18.6c
     40    19.9    9.5    15.6   7.3    17.0    8.0    20.0    54 of 229   47.5    8.7         1.37 /  8.6           19.1
     80    24.3   10.2    17.5  10.7    15.8    7.2    26.2    84 of 384   42.4   11.4         1.46 / 10.1           28.0
    120    24.4   11.2    17.0  11.7    15.6    7.2    29.0    89 of 443   42.9   12.8         1.48 / 10.8           31.6
    150    24.8   11.4    16.7  12.2    15.8    7.1    31.0    94 of 455   42.6   13.6         1.51 / 11.0           32.0

  - THE ENGINE DOES WHAT IT IS TOLD: unintended motion is 1.2-1.5c median,
    7-11c p90, at every tau. The activity IS the commanded correction.
  - GLIDE (the envelope in transit) is the largest bucket at every tau and
    the one that grows most with tau in absolute terms (6.5 -> 13.6c): a
    slow retune spends its time between source and target. That is the
    CHARACTER region §17.4 describes, in these units.
  - SNAP (the note-change chase + confirmation snap) is 34% at tau 6 and
    20-25% above; its in-window peak grows 18.6 -> 32c with tau. It is
    still the tau-scaling mechanism; it is a third the size claimed above.
  - DETECT is now the GENUINE residual timing error: 6-8% of hops, 15-17%
    of activity, and at tau 6 it holds 38 of the 111 events over 25c - a
    third of the fast end's events. Tau-independent in cause; its per-hop
    mean grows with tau only because the chase adds to it in the same hops.
  - Sean's 3s event, aligned (tau 150, 3.24-3.31s): the corrector holds the
    OLD note as target for ~60ms after the audio has moved (target-src
    -34 -> -76c through the confirm window), commanded follows (-28 ->
    -71c), output follows commanded (-31 -> -61c), then the confirm snap
    (3.301s) brings it home in two hops. f0Here-src is -13..-20c inside
    the fastest 20ms of the glide - the real lag - and here it REDUCES the
    excursion (a lagging f0 on a rising glide makes the ratio less
    negative). At tau 6 the same boundary shows -24c then a +29c overshoot:
    the new target pulling a still-gliding source (+26c intended) plus the
    lag's +12c. The chase is the event; the lag is a ~10-20c modifier.

## The diagnosis (ruling 2): NOT (a); (b) plus (c), both measured

Lead in MILLISECONDS is constant across glide rates at every observer
(within 0.3 ms from 0.25 to 2 c/ms), so it is an alignment offset. It
varies with SUNG PITCH and with VOICE TYPE:

    shifter residual lag    110 Hz   165 Hz   250 Hz   330 Hz     (alto_tenor)
                            -5.2     -6.5     -7.4     -7.9 ms
                             80 Hz   110 Hz   160 Hz              (low_male)
                            -8.3    -10.0    -11.4 ms

The design (EedPitchEngine.h pitchLagFor) back-dates by frameLen/2 + one
hop: "a frame spanning [p - frameLen, p] describes the middle of that
span". Two things it misses:
  (b) CENTROID, MIS-MODELLED: the frame is W + tauMax + 2 long, but YIN's
      difference at lag tau correlates the span [s, s + W + tau); its
      centroid is s + (W + tau)/2, not s + frameLen/2. The shortfall is
      (tauMax - tau)/2 + 1 samples - ZERO at the voice type's lowest note,
      tauMax/2 (6.25 ms alto, 9.1 ms low) at the top. Predicted span across
      110 -> 330 Hz alto: 3.0 ms; measured: 2.7 ms.
  (c) PIPELINE HOPS: the remainder is constant per voice type, 3.2 ms alto
      / 5.5 ms low. One hop of it (2.67 ms) is the median-of-3 publish
      stage (analyseHop step 6), a one-hop group delay pitchLagFor does not
      count. The rest (~0.5 ms alto, ~2.8 ms low) is unidentified; the
      low_male decimation stage is the candidate.
  (a) OVER-CORRECTION: refuted. The compensation UNDER-corrects; nothing
      subtracts too much. The clamp min(pitchLag, latency-1) is inactive
      (1180 < 1799, 1660 < 2618).
Model check: lag = frameLen - (W + tau)/2 + 2 hops predicts the raw hop
lag at 1463 samples vs 1469 measured (alto, 165 Hz) and 2004 vs 2026
(low, 110 Hz): within 0.1-0.5 ms.

## The proposal (NOT built) and its bar

Fix shape: make the ring-write back-dating per hop, from the hop's own
period: lag_h = W/2 + tauMax + 2 - tau_h/2 + 2*hop (tau_h = fs/f0_h),
in place of the constant frameLen/2 + hop. Same site (the f0 ring write in
PsolaEngine::process, `lag`), no new state; the corrector's hop cadence is
untouched. Both latencies leave headroom for the largest lag_h.

THE BAR, before a line of engine code:
  1. tools/pitch_lead_probe: shifter residual |lag| <= 1.0 ms at 110/165/
     250/330 Hz alto_tenor and 80/110/160 Hz low_male, rates 0.25-2 c/ms
     (4 c/ms informational: the estimator itself saturates there). Steady-
     note f0Here error unchanged (<= 0.15c).
  2. sourceNEW, hard, ign OFF, seam 60, tau 6 (the setting Sean uses): the
     DETECT bucket's >25c count falls from 38 and its share of activity
     from 14.8%; total >25c events do not rise.
  3. Paired per-instant sustain delta median <= +0.15c, no contiguous >50ms
     region worsening >2c (the standing sustain clause), both vib modes.
  4. Onset off-grid MEDIAN not regressed, paired; tails reported.
  5. OLD take falsifier: ign-vib ON word-start events stay 0.
  6. Sean's ear on the 3.2s and 3.05s boundaries at tau 6 - hard gate.
Ruler discipline for the bar: every tap comparison aligned by the measured
ruler lag (PA_RULER_LAG), or against synthetic truth. Never the raw track.

## 6. KeyEngine auto-key (ruling 3): what accumulates, what resets, what a stale key does

DEFAULTS: EchoJay Pitch ships key_auto ON and reference_auto ON
(EedPitchProcessor.h keyAuto_/refAuto_ {true}); choosing a key manually
turns key_auto off. Whether Sean's session is on auto is not readable from
here - the [DETECTED KEY] readout in his session answers it.

THE FEED: one process-wide singleton (KeyFeed), published from the plugin
processor's 1 Hz wall-clock timer, last writer wins across instances.
Precedence (collectKeySources): keyed capture (fresh <= 15 min) > bus Link
> "this channel" when its role is declared a music bus > channel Link >
local chain; a vocal-channel local reading is POISONED (never auto-used).
usable() = valid && confidence >= 0.50. AGE IS IGNORED except for captures:
a bus or self reading of any age is applied with full confidence.

WHAT ACCUMULATES (EedKeyEngine): a 32 s ring of decimated audio at 16 kHz;
in continuous mode a fresh analysis every 2 s of NEW audio over up to the
window (default 10 s), with hysteresis (the incumbent keeps the seat unless
the challenger wins by a sensitivity-scaled margin) and a hold switch.
Silence: waitingForSignal - the previous reading is KEPT ("a pass that
completed over nothing tonal is reported rather than silently ignored").
NOTHING RESETS ON TRANSPORT: no playhead read anywhere in the key path;
reset() / clearAccumulation() run only from the device's reset control.
Because passes need 2 s of new audio, a stopped transport freezes the key
at its last value indefinitely, and a bounce - whose 1 Hz publish runs in
wall time while audio runs faster - inherits whatever was live. Live and
bounce therefore see the SAME key unless the live key changed after the
bounce, or the bounce started under a different reading.

WHAT A STALE OR WRONG KEY DOES TO THE TARGET: refreshAutoKey applies
root + major/minor via applyScale with a cross-fade; nearest-enabled-degree
selection then pulls every note outside the wrong scale a semitone to the
nearest wrong degree. Measured historically (the confidence-gate comment):
correcting to a wrong key pushed a take from 13c off the nearest note to
29c. That is a TAKE-SCALE failure - every out-of-scale note, the whole
take, exactly the shape of "a whole take sounding wrong live". Mode errors
are the cheap case: D minor vs D major differ on three degrees (F/C/Bb vs
F#/C#/B); relative major/minor share notes and are harmless.

THE GAP THIS INVENTORY FOUND: the circularity guard covers the TUNING
REFERENCE only (refCircular -> 440). The KEY ROOT AND MODE from a self-
derived fact are applied unguarded. A vocal channel whose role is
(mis)declared as a music bus keys the corrector off the singer's own
melody - the third instance of the machinery this project has already
shipped two defects in. Not measured; not in the offline chain. THE CHECK
FOR SEAN (cheap, before anything else): with the take playing live, read
the [DETECTED KEY] block - source name, root/mode, confidence, age - and
compare it with the key the bounce was made under. If they differ, or the
source is "this channel", that is the live-only complaint.
