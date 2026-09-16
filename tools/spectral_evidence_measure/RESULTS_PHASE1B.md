# Phase 1b: the BEFORE numbers

**These are the BEFORE numbers.** They were taken on **16 September 2026**, at commit
**6e4c1db** ("Name the band scheme and the ballistics, so a boundary cannot move
silently"), before any line of the macro band accumulation was written, with the same
harness that will take the AFTER numbers. That is what makes the two comparable by
construction rather than by hope.

Everything here is produced by `tools/spectral_evidence_measure`, which drives the
**shipped** `MeterEngine` and `ReferenceAnalyser`. It is read only on the user's audio:
45 files were opened for reading and nothing was written beside them. The only file the
harness writes anywhere is its own synthesised control tone, in the system cache
directory.

Reproduce with:

```
tools/spectral_evidence_measure/build_and_run.sh --controls 12
tools/spectral_evidence_measure/build_and_run.sh --census ~/Documents/EchoJay/References
tools/spectral_evidence_measure/build_and_run.sh "<reference>" "<capture>"
```

---

## 1. Control A: deterministic multi-tone. This one gates.

Fourteen tones, all on exact FFT bin centres (multiples of 21.5332 Hz at 44.1 kHz),
several in every macro band, none near a band edge, constant amplitude, no fade,
60 seconds, a length that is an exact multiple of 2048 samples.

| band | k | Hz |
|---|---|---|
| sub | 2 | 43.07 |
| low | 5, 9 | 107.67, 193.80 |
| lowMid | 15, 20 | 323.00, 430.66 |
| mid | 30, 50, 80 | 646.00, 1076.66, 1722.66 |
| highMid | 110, 180, 250 | 2368.65, 3875.98, 5383.30 |
| air | 320, 500, 800 | 6890.62, 10766.60, 17226.56 |

**Why bin centres.** `computeSpectrum` refills a 2048 sample ring with each 2048 sample
block, so `fftWritePos` returns to the same value every block and the window lands on the
same phase. A tone periodic in 2048 samples therefore produces an identical frame every
block: frame to frame variance is not small, it is zero. A tone off a bin centre would
leak differently as its phase walked, putting back the variance this control exists to
remove. The length is an exact multiple of 2048 because a partial final block shifts the
ring alignment and its frame would legitimately differ from every other one.

**Why sub gets one tone.** At 44.1 kHz the sub band contains only two FFT bins, and the
lower of them sits on the 20 Hz edge.

### The bound, derived before the run

On a perfectly periodic signal the only source of tail versus whole file difference is
the initial settling transient. From a -120 dB start to a -30 dB level the attack pole
(coefficient 0.990381 at 2048/44100) contributes a total deviation of 0.874 dB spread
across 1292 blocks. float32 quantisation at -30 dB adds about 4e-6 dB per sample and the
accumulators are double.

| | |
|---|---|
| modelled bound | **0.00068 dB** |
| measured, worst absolute | **0.00071 dB** |
| **ratio** | **1.04** |
| measured, worst relative | 0.00003 dB |
| gate | 0.05 dB, about 50x the bound, to cover what was not modelled |
| verdict | **PASSES**, at 0.7x the modelled bound, nowhere near the gate |

The arithmetic predicted the answer to within five percent. That is a stronger statement
than the gate passing, and it is the reason the harness is trusted rather than believed.

### Control A, both tables

```
                                        sub      low   lowMid      mid  highMid     high
tail  ABS  (last block, ~450ms)      -31.49   -28.48   -25.69   -26.94   -25.93   -26.32
last500 ABS                          -31.49   -28.48   -25.69   -26.94   -25.93   -26.32
whole meanDb ABS                     -31.49   -28.48   -25.69   -26.94   -25.93   -26.32
whole meanPow ABS                    -31.49   -28.48   -25.69   -26.94   -25.93   -26.32
DIFF  whole meanDb - tail             -0.00    -0.00    -0.00    -0.00    -0.00    -0.00

tail  REL                             -4.02    -1.00    +1.79    +0.54    +1.55    +1.15
whole meanDb REL                      -4.02    -1.00    +1.79    +0.54    +1.55    +1.15
DIFF  REL whole - tail                +0.00    +0.00    -0.00    -0.00    -0.00    -0.00
```

---

## 2. Control B: pink noise across twelve seeds. This one measures.

Twelve seeds, 60 seconds each, same accumulation. This does not gate anything.

| band | bins | mean ABS | **sd ABS** | max abs | **sd REL** | max rel | sd/air |
|---|---|---|---|---|---|---|---|
| sub | 2 | -0.412 | **2.241** | 4.384 | **1.897** | 3.447 | 12.71 |
| low | 9 | -0.029 | **1.392** | 3.041 | **1.344** | 2.799 | 7.90 |
| lowMid | 12 | 0.095 | **1.238** | 2.909 | **1.151** | 2.236 | 7.02 |
| mid | 69 | -0.295 | **0.423** | 0.943 | **0.533** | 1.034 | 2.40 |
| highMid | 186 | 0.017 | **0.297** | 0.522 | **0.537** | 1.078 | 1.68 |
| air | 650 | -0.105 | **0.176** | 0.427 | **0.395** | 0.617 | 1.00 |

**The hypothesis, written before the table existed:** the disagreement is estimator
variance, a band averages over however many FFT bins fall inside it, so spread should
fall as one over the square root of the bin count. Sub was required to show the largest
spread, which an earlier single run contradicted at 0.19 dB.

Predicted sd/air: sub 18.0, low 8.5, lowMid 7.4, mid 3.1, highMid 1.9, air 1.0.

**The ordering is exactly right**, six for six, monotone in bin count with no tie out of
place. **The magnitudes are systematically low**: every band comes in under prediction,
sub by the most at 12.7 against 18.0. The mechanism is confirmed in direction and
unconfirmed in magnitude. No second term has been invented after the fact to close
the gap.

**Sub does show the largest spread**, 2.24 dB with a worst case of 4.38 dB across seeds,
so the earlier 0.19 dB was a lucky draw and not a counterexample.

### The measured noise floor

One standard deviation, absolute, per band. **This is the number that should have been
the threshold all along:**

| sub | low | lowMid | mid | highMid | air |
|---|---|---|---|---|---|
| 2.24 dB | 1.39 dB | 1.24 dB | 0.42 dB | 0.30 dB | 0.18 dB |

A single flat 1 dB was **simultaneously far too tight for sub**, where it sits at 0.45 of
a standard deviation, **and five times too loose for air**. One number could never have
worked. An earlier run of this work failed a 1 dB control at 1.217 dB in low, which sits
inside low's measured spread of 1.39 dB: that control did not fail because the harness
was wrong, it failed because the threshold was below the noise floor of the thing being
measured.

**This reaches past the control.** The 2 dB flag threshold in `appendTonalDiff` sits below
one standard deviation of the measurement in three of the six bands.

---

## 3. The census: 45 files

Every file in `~/Documents/EchoJay/References`, read only, through the shipped
`MeterEngine`. Classified by how many of the six **tail** bands sit above -100 dB:

| class | rule | count |
|---|---|---|
| strictly live | all six tail bands above -100 dB | **13** |
| strictly dead | zero tail bands above -100 dB | **14** |
| partial | between one and five tail bands alive | **15** |
| near silent throughout | no whole file band above -100 dB, nothing to compare | **3** |
| | | **45** |

**14 dead plus 15 partial is 29 of 45 references whose tail is wholly or partly on the
floor.** Phase 0b reported six of eight dead against a library of eight; against 45 the
proportion has fallen from 75 percent to 31 percent, but the failure is still the rule
rather than the exception.

**A correction to an earlier count.** During the session this was first reported as 16
partial and 2 near silent. `VITO (UK), Casazona Stems 126 Bpm Aminor Reverse Crash.wav`
has a partly live tail but **no whole file band above -100 dB**, so there is nothing to
compare it against and it belongs in the near silent class. The correct split is 15 and 3.

### Strictly live, all six tail bands alive (13)

| file | seconds |
|---|---|
| 11 FEB  - LANCEY FOUX [OPEN].wav | 140.6 |
| 11 LOCKED UP 44.1K 24Bit.wav | 168.5 |
| Casazøna X Vito - FEEL YOUR MIND.mp3 | 365.7 |
| direct 135bpm prod by hl8 x kh.mp3 | 184.9 |
| Free Slime&Sezy Girl + Skit Demo.mp3 | 278.2 |
| M Huncho X Morad V1.wav | 103.7 |
| More Time Bpm 137 (Murdz x H&K).mp3 | 196.3 |
| PLEASER 144BPM - PROD BY HL8 X HARRY BEECH X JONY.mp3 | 186.7 |
| Print_50 1.wav | 204.3 |
| Reference song 1.wav | 151.2 |
| Skrapz - I'm Yours Master 2.wav | 159.1 |
| Tom X Zakhar X 169 1.mp3 | 170.7 |
| Wild & Free Rough mix.mp3 | 154.8 |

### Strictly dead, every tail band on the floor (14)

| file | seconds |
|---|---|
| 02 CHIP FT NAFE SMALLZ  - WOW (EXPLICIT) 15-04-24 [MASTERED_MP3] 2.mp3 | 152.2 |
| 07 WHO DECIDES WAR - Master 24b44k.wav | 136.1 |
| AUDIO-2026-04-24-12-42-02.mp3 | 88.3 |
| Commerical test song.mp3 | 153.5 |
| DAPPY - STRAIGHT FACTS PART 2 Remix 2.wav | 166.4 |
| DAPPY - STRAIGHT FACTS PART 2 V1.wav | 196.2 |
| DAPPY - STRAIGHT FACTS PART 2.mp3 | 172.4 |
| DAPPY SF2 LOGIC FINAL BOUNCE.wav | 202.7 |
| ELEVATION 144bpm - Prod by HL8 x KH.mp3 | 188.4 |
| house 001 intro 2.wav | 10.3 |
| house 001 intro.wav | 10.3 |
| MORRISSON_SILVER SPOON_MAIN MIX_V7_11_09_26.wav | 378.3 |
| Mugzz- Uh Uh.mp3 | 153.5 |
| SINN6R OPEN.wav | 147.1 |

### Partial, the dangerous class (15)

These are the references where `computeCompareFig` takes the mean over the surviving
bands only and presents it as a six band relative. See section 7.

| file | seconds | tail bands alive |
|---|---|---|
| Ain't No Sunshine When She's Gone.mp3 | 235.2 | 1 of 6 |
| Chip & Nafe Smallz - Neighbourhood - Master 44k16b.wav | 197.7 | 4 of 6 |
| Giggs x Tion Wayne - Stevie Wonder Print.wav | 228.9 | 4 of 6 |
| Hamaza X Millionz X Pomp V1.wav | 192.8 | 4 of 6 |
| K1 - NFL - 24 Bit Master.wav | 209.6 | 5 of 6 |
| LC X UNKNOWN T - HOCUS POCUS_ACC_WET.wav | 202.9 | 1 of 6 |
| Martinez Brothers - Just A Feeling - Deliverables - 24 Bit Master.wav | 190.2 | 3 of 6 |
| NATURAL HABITAT v7 .mp3 | 173.5 | 2 of 6 |
| old mix v7 .mp3 | 173.5 | 2 of 6 |
| Perspecasity 152bpm - R14 x Sean Murdz x H&K.mp3 | 177.9 | 4 of 6 |
| rolling 130bpm prod by hl8 x kh.mp3 | 221.6 | 5 of 6 |
| Skrapz - Check 24 Bit 96K Master (FInal).wav | 148.5 | 5 of 6 |
| Steel Banglez Feat. M24 - Feelings V1.wav | 172.2 | 3 of 6 |
| Unknown T - Wisdom (No Chops).wav | 122.2 | 1 of 6 |
| _ACC_WET.wav | 202.9 | 1 of 6 |

### Near silent throughout, nothing to compare (3)

| file | seconds |
|---|---|
| aritst - its the way V1.aif | 68.6 |
| VITO (UK), Casazona Stems 126 Bpm Aminor Piano.wav | 365.7 |
| VITO (UK), Casazona Stems 126 Bpm Aminor Reverse Crash.wav | 365.7 |

---

## 4. The pair

| role | file | why |
|---|---|---|
| live tail | `Reference song 1.wav`, 151.2 s | all six tail bands alive, and a genuine fade: tail sub -47.81 against whole file -16.75 |
| fades to silence | `02 CHIP FT NAFE SMALLZ - WOW`, 152.2 s | all six tail bands at exactly -120.00 with a healthy whole file, the cleanest division by nothing in the library |
| capture side | `Wild & Free Rough mix.mp3`, 154.8 s | held **identical** across both rows, so any difference between them is attributable to the reference alone |

### Live tail: Reference song 1.wav, 3543 blocks over 151.2 s at 48 kHz

```
                            sub      low   lowMid      mid  highMid     high
  tail ABS               -47.81   -60.27   -94.52   -91.97   -94.22   -95.04
  last500 ABS            -40.00   -51.79   -91.42   -89.49   -94.72   -94.87
  whole meanDb ABS       -16.75   -21.60   -24.86   -27.42   -29.58   -31.17
  whole meanPow ABS      -15.87   -20.35   -22.60   -24.98   -26.63   -27.05
  DIFF whole - tail      +31.07   +38.67   +69.66   +64.55   +64.64   +63.87
  in units of sd ABS       13.9x    27.8x    56.3x   152.6x   217.6x   362.9x

  tail REL               +32.83   +20.37   -13.88   -11.33   -13.58   -14.40
  last500 REL            +37.05   +25.26   -14.37   -12.44   -17.67   -17.83
  whole meanDb REL        +8.48    +3.63    +0.37    -2.19    -4.36    -5.94
  DIFF REL               -24.34   -16.74   +14.25    +9.14    +9.23    +8.46
  in units of sd REL       12.8x    12.5x    12.4x    17.1x    17.2x    21.4x
```

Windows: tail 1 block (0.043 s, about 450 ms of ballistic memory); last500 12 blocks
(0.512 s); whole 3543 blocks (151.2 s).

**Every difference is between twelve and three hundred sixty three times the measured
noise floor.** Nothing here is a sampling artefact.

### Dead tail: 02 CHIP FT NAFE SMALLZ, 3279 blocks over 152.2 s at 44.1 kHz

```
                            sub      low   lowMid      mid  highMid     high
  tail ABS              -120.00  -120.00  -120.00  -120.00  -120.00  -120.00
  last500 ABS           -120.00  -120.00  -120.00  -120.00  -120.00  -120.00
  whole meanDb ABS       -35.43   -22.37   -27.33   -25.45   -29.66   -34.80
  whole meanPow ABS      -17.87   -15.35   -22.57   -21.43   -24.70   -28.41
  DIFF whole - tail      +84.57   +97.63   +92.67   +94.55   +90.34   +85.20
  in units of sd ABS       37.7x    70.1x    74.9x   223.5x   304.2x   484.1x

  tail REL                +0.00    +0.00    +0.00    +0.00    +0.00    +0.00
  whole meanDb REL        -6.25    +6.80    +1.84    +3.72    -0.49    -5.63
  DIFF REL                -6.25    +6.80    +1.84    +3.72    -0.49    -5.63
  in units of sd REL        3.3x     5.1x     1.6x     7.0x     0.9x    14.3x
```

**Read the last row carefully.** On the dead reference two of the six *relative*
differences are at or inside the noise floor, highMid at 0.9 sd and lowMid at 1.6 sd.
The absolute differences are enormous, 38x to 484x, but the relatives are not uniformly
significant, because a dead tail's relatives are zero by construction rather than wrong
by a measured amount. The absolutes are the significant finding here, not the relatives.

**Those `+0.00` tail relatives are the harness's representation, not the plugin's.**
`computeCompareFig` (`PluginProcessor.cpp:3343`) counts only bands above -119 dB, finds
none, leaves `bandValid` false, and the card and prose render "N/A". For a fully dead
tail the shipped code refuses correctly.

---

## 5. The prediction, and the verdict on both clauses

Written and printed before any number from the pair was read:

> a fade out tail is quieter and duller than the whole file average, so the whole file
> figures should show less extreme relatives than the tail, with the largest corrections
> in sub and air

**First clause: HELD, decisively.** On the live reference the tail relatives span +32.83
to -14.40, a range of 47 dB. The whole file relatives span +8.48 to -5.94, a range of
14 dB, a third as extreme. The plugin today describes that record as having **33 dB more
sub than its own average**; over the whole performance it has **8.5 dB more**.

**Second clause: FAILED.** The largest correction is sub at 24.34 dB as predicted, but
**air is the smallest at 8.46 dB**, not among the largest. The corrections rank sub, low,
lowMid, highMid, mid, air. The reason is visible in the absolutes: the tail on this record
is a low frequency fade rather than a broadband one, so the high bands were already near
their whole file relationship and had little to correct.

**Consequence.** The shape of the correction is material dependent, so a claim about
where the correction lands cannot be made from one file.

**The prediction does not apply to the dead reference at all**, and scoring it as held
there would be false. A dead tail's relatives are not less extreme, they are nothing.

---

## 6. The four falsifiers

| # | falsifier | live | dead |
|---|---|---|---|
| 1 | all six absolute differences within 0.5 dB | does not hold | does not hold |
| 2 | sub or air moves less than mid, **in relatives** | **FIRES**: sub 24.34, mid 9.14, air 8.46 | does not hold: sub 6.25, mid 3.72, air 5.63 |
| 3 | the two band schemes disagree in **sign** on the same file | 0 of 6 | 0 of 6 |
| 4 | whole file macro relatives against `computeBands(eqCurve)` | gap to 7.27 dB in air | gap to 6.68 dB in air |

**Two of these were miswritten when they were specified, and both were rewritten before
the final run rather than explained away.**

**Falsifier 2 tested the wrong quantity.** The prediction it guards is about relatives;
the falsifier compared absolutes. It fired on both references for a reason that had
nothing to do with whether the prediction was right, because sub moves least in absolute
terms and most in relative terms on the same audio. Both statements are true of the same
data, which is how a check can look meaningful and measure nothing. It now compares
relatives, still fires on the live reference, and that firing is informative: it is the
same fact as the second clause of the prediction failing.

**Falsifier 3 could not fail.** It asked whether a relative changed sign between the tail
and the whole file. On a dead tail every relative is exactly zero, so the test reduced to
asking whether the whole file relative is positive, and it reported three of six on the
reference it was designed for. It is replaced with a test that can fail and that measures
what decision 8 will be judged on: whether the macro scheme and the bin scheme agree in
sign about the same file.

**Falsifier 4 is not a pass or a fail.** It records the size of the known structural
difference between the two schemes, so the unification can be judged against a number
instead of against nothing.

### Falsifier 4, both files

```
LIVE                        sub      low   lowMid      mid  highMid     high
  macro whole REL         +8.48    +3.63    +0.37    -2.19    -4.36    -5.94
  computeBands REL       +12.28   +10.06    +2.47    -3.14    -8.46   -13.22
  GAP                     -3.79    -6.43    -2.10    +0.95    +4.10    +7.27

DEAD
  macro whole REL         -6.25    +6.80    +1.84    +3.72    -0.49    -5.63
  computeBands REL        -0.97    +9.78    +4.92    +2.02    -3.45   -12.30
  GAP                     -5.29    -2.98    -3.08    +1.70    +2.96    +6.68
```

The two schemes differ by up to 7.3 dB on the same audio, largest in air and low, exactly
where their edges and the display tilt differ most. **They never disagree in sign on
either file**, so they tell the same directional story at different magnitudes.

---

## 7. The partial tail, which is worse than the dead tail

A **fully dead** tail refuses. `computeCompareFig` finds no band above -119 dB, leaves
`bandValid` false, and the card and the prose render "N/A". Wrong window, honest output.

A **partial** tail does not refuse. With some bands alive and some on the floor, `n` is
greater than zero, `bandValid` becomes true, and the mean is taken over the **surviving
bands only**. A four band mean is then presented as a six band relative, with the floor
bands reported as -999. That is not a refusal and not a correct figure: it is a real
number computed against the wrong denominator, rendered with the same confidence as a
good one.

**This ships on 15 of 45 references.** It is the worst of the three cases and it was not
on anyone's list before this measurement, because from outside it looks like the working
case.

---

## 8. float against double, measured

The harness checks its own loop against the shipped `ReferenceAnalyser` by comparing the
`eqCurve` both produce. If the loops differ, the whole file figures are not the ones the
accumulation will produce and BEFORE and AFTER stop being comparable.

**They are not bit identical, and the reason is in the shipped code.**
`ReferenceAnalyser.cpp:137` accumulates into `std::array<float, 64>`; this loop
accumulates into `double`.

| reference | frames | derived threshold | measured worst | bins over |
|---|---|---|---|---|
| Reference song 1.wav | 3543 | 0.025342 dB | 0.000111 dB | 0 of 64 |
| 02 CHIP FT NAFE SMALLZ | 3279 | 0.023453 dB | 0.000134 dB | 0 of 64 |

**The threshold is derived, not picked.** float32 half ulp rounding is
`eps = 2^-24 = 5.96e-8` relative. Each addition rounds a running sum whose magnitude is
bounded by `frames * 120`, because the spectrum floor is -120 dB, so the worst case error
after dividing by `frames` is `eps * 120 * frames`. With random rather than adversarial
signs the expectation is `eps * |v| * sqrt(frames^3 / 3) / frames`, which for a typical
50 dB magnitude and 3279 frames is **9.9e-5 dB**. The measurements are 1.1e-4 and 1.3e-4,
so the observation sits where the realistic estimate says it should, and the gate is set
at the rigorous worst case because a check must not fire on arithmetic that is provably
correct.

An earlier version of this check used a flat 0.0001 dB and fired on both references. That
was the check being too tight, not the loop being unfaithful.

**The consequence for Phase 1b is a choice, not a defect.** At 1.3e-4 dB the effect is
four orders of magnitude below the smallest noise floor measured here, 0.18 dB in air. But
if the macro accumulation is added to `ReferenceAnalyser` with a float accumulator it
inherits this, and a double costs nothing.

---

## 9. The AFTER test, designed and recorded before the accumulation exists

Written down now so it cannot be chosen to suit the result later.

**The primary AFTER test is Control B re-run with the same twelve seeds.** Once the
accumulation lands, the figure the plugin reports for a band stops being one ballistic
reading and becomes a mean over every frame of the window. If the frames were independent,
the spread of that figure across seeds would fall by the square root of the frame count,
`sqrt(1292) = 35.9`.

| band | BEFORE sd (the tail) | PREDICTED sd (tail / sqrt f) | achievable now (this harness) |
|---|---|---|---|
| sub | 2.261 | 0.063 | 0.132 |
| low | 1.383 | 0.038 | 0.044 |
| lowMid | 1.255 | 0.035 | 0.042 |
| mid | 0.423 | 0.012 | 0.018 |
| highMid | 0.298 | 0.008 | 0.016 |
| air | 0.176 | 0.005 | 0.005 |

**The correlation caveat, stated now rather than after the result.** The third column is
this harness computing the accumulation today, and it sits between **1.0 and 2.1 times
above** the square root prediction, because consecutive frames are not independent: the
ballistic smoother correlates them. So the honest pre-registered expectation for the
shipped accumulation is a fall of **roughly 17x to 36x**, not exactly 35.9x. A result
inside that band is a pass. A result near 1x means the accumulation is not reaching the
reported figure.

**The direction of change on the pair is secondary.** It is one file and one story about
it. Control B is twelve independent draws and a number.

---

## AFTER

*Empty by design. The next run fills this section in place, so the diff of this file is
the result. Do not delete the BEFORE sections: the comparison is the point.*

### A1. Control A after the accumulation

*(to be filled)*

### A2. Control B after the accumulation, same twelve seeds

*(to be filled: per band spread, and the fall factor against the 17x to 36x expectation
recorded in section 9)*

### A3. The pair after the accumulation

*(to be filled: both tables, with the same noise floor quoted beside every difference)*

### A4. The four falsifiers after the accumulation

*(to be filled)*

### A5. Did the pre-registered prediction hold

*(to be filled)*
