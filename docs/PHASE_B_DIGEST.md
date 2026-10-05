# Phase B — the digest for Sean (drafted 5 Oct 2026; Kathy decides when it goes)

Nine measurement modes beyond the compressor profile, each a prototype in `ejmap` (branch `feat/ejmap-cert`), nothing
exported or published. Every one runs the same way: the probe hosts the plugin in its own process, EJ Map writes the
controls and reads the output, and a name only NOMINATES a control — it keeps the role when its measured effect shows the
role's signature; an unnamed control that shows it is reported. Numbers below are from this Mac; every figure has its trace.

## Gain and output controls

**Method.** Every gain-role control at 21 positions, a 997 Hz sine at −20 / −40 dBFS (input gains also at −60, and judged
there); the output's level change against the label, relative to the control's own 0.0 point, judged at the label's resolution.

**Findings.** 1. Most labels are honest to 0.0–0.1 dB (bx_opto, 254E, SBC Input / Output, mpressor, Zip). 2. Two are not:
Solid Bus Comp's Makeup 1.0 dB off, bx_townhouse's MakeUp 6.8 dB off. 3. An input gain inside the compression path is
level-dependent by up to 7 dB (SBC's "Gain"): at −60 the label is honest, at −40 it is already compressing — the record
now says which, and the role step drops a "gain" that moves differently at two levels (a path, not a gain).

**Questions.** Where does gain calibration live in the profile — a per-control `label_offset_db`, or a corrected map?
Should a path-type gain (SBC's Gain) be excluded from writes, or written with its measured law?

## Compressor timing

**Method.** A tone burst stepping 10 dB over the amount's 1 dB point (quiet 6 dB under it), 5 ms windows aligned by the
plugin's latency; attack = 63 % of the GR step, release = 63 % recovery; the loud and post segments scale to the label
(5 ×). Program dependence from a 0.3 s against a 3 s burst.

**Findings.** 1. Attack / release track their labels by each unit's own definition (mpressor's release label IS the 63 %
time; 254E's ~2×; RComp's ~0.5×). 2. SBC's Release 3.000 measures 2066 ms, its Attack 1 → 30 ms reads 8.8 → 48 ms; SBC is
program-dependent (×2.6). 3. Latency alignment was the whole fix — without it a step window reads a +16 dB spike.

**Questions.** Which definition the server wants (63 %, or the vendor's full rise / fall), and whether program dependence
should be a flag or a second figure.

## Limiters

**Method.** A −1 dBFS sine into the amount's hard end (decided by measurement), the ceiling at the labels nearest −0.1 /
−0.3 / −1 / −3 / −6, oversampling off and on; the output's sample peak and a 4 × cubic true peak against the label.

**Findings.** 1. MLimiterX overshoots true peak by +0.58 dB at every ceiling (sample peak exact). 2. L2, bx TP and Elevate
hold both. 3. A ceiling the drive never reached is "not driven", not a pass; the role step tells a ceiling (holds the
peak against a 6 dB drive change) from a post gain (passes it).

**Questions.** Sample peak or true peak as the ceiling the server trusts? Should the profile carry the oversampling
state the figure was measured in?

## EQs

**Method.** A 121-tone log multitone (1/12 octave) at −12 dBFS; per band the gain, frequency and Q sweeps read against the
unit's own no-write baseline; centre (parabolic), gain, −3 dB bandwidth, shelf corners; a flat band gets its enable switches
tried, closest name first.

**Findings.** 1. bx_digital's labels hold within 0.5 %, with proportional Q. 2. AMEK EQ 200's centres sit 4–10 % below
their labels; PEX-500's "Low boost 0–10" is not dB (10 → 15 dB) and its 30 / 60 / 100 Hz are turnovers (corners 211 / 309 /
663 Hz); BAX labels 1.2 ×. 3. bx_digital's "2" bands stay flat with their Active switches on: they are the Side channel, and
an identical stereo input has no Side — the test signal needs a decorrelated pair for M/S units.

**Questions.** What shape the server wants per band (centre / gain / bandwidth, or corner for shelves), and whether an
M/S unit's second channel is in scope.

## De-essers

**Method.** The compressor ladder with the tone at 6.5 kHz (997 Hz as the control), the threshold at 6 norms over five
levels; the same on band-limited noise 4–10 kHz; the reduction's shape on a multitone at the hardest threshold — a notch's
centre or a shelf's half-depth corner — against the frequency label; split band against wideband from the cut at 997 Hz.

**Findings.** 1. Band selectivity is clean on every working de-esser: 5–19 dB at 6.5 kHz against 0.0 at 997 Hz; the noise
reads the same as the tone within 0.7 dB. 2. The Waves "Freq" labels are corners, not centres (the cut holds to 20 kHz):
DeEsser's corners sit 13–35 % from the label. 3. Lindell 902's HF Only switch is literal: wideband off (everything −13 dB),
split band on (a 7 kHz corner).

**Questions.** The frequency figure: the label (detector), the corner, or the centre? Mode in the profile as measured?
Threshold → GR at which level — the ladder has five.

## Saturation

**Method.** One 997 Hz sine (an exact bin) at −20 / −12 / −6 dBFS, the drive control at 11 norms; the fundamental's gain and
the 2nd–5th harmonics by exact Goertzel bins: THD, even/odd character, the 1 % and 0.1 % onsets.

**Findings.** 1. Every onset is level-dependent (J37's 1 % point: '18.7' at −20, '9.4' at −12, '4.0' at −6). 2. The gain
law and the THD law are separate things (karacter loses 14 dB reaching 33 % THD; MSaturator's gain is constant); character
moves along the drive (BIG AL even → odd). 3. Three shapes that are not drive laws: inert, silent (bx_yellowdrive, freqtube:
no output at all) and no effect (MSaturator's per-harmonic trims, NLS Buss's VCA drives).

**Questions.** THD at 997 Hz, or a feel ladder (clean / warm / driven / crushed)? Which level the server assumes when it
writes a drive — the profile could carry the onset per level.

## Reverb and delay

**Method.** A burst then 6 s of silence (the tail scaled to a decay label), the output per 1 ms window; dry in the burst's
first 2 ms, wet after it (or at the first repeat); the mix law over 11 positions against linear and equal power; RT60 by
T20 / T30; pre-delay and delay time against labels; repeats as rising edges, the fall per repeat against 20 log10 (feedback %);
tempo sync at 90 / 120 / 140 from a playhead the probe supplies.

**Findings.** 1. The mix law differs by product: H-Delay and ValhallaVintageVerb equal power to 0.00 dB, MannyM Delay linear
to 0.04, H-Reverb a send law — "20 % reverb" is −1.9 dB of dry on one unit and 0 dB on another. 2. Time labels hold to 2 %
where they are ms (bx_delay2500, Abbey Road Plates to 0.5 ms, H-Delay under 1.5 s); H-Delay syncs to the playhead exactly.
3. Feedback % is not amplitude % (bx_delay2500: 1.5 dB per repeat less fall than 20 log10 at every setting); H-Delay's
feedback above 100 grows the repeats.

**Questions.** Does the server write mix as a percentage or as a wet level below dry (the law decides what 20 % means)?
RT60 at 1 kHz or broadband? Delays with a note control and no sync switch (MannyM): carry the note values as the time map?

## Transient shapers and gates

**Method.** A drum-like burst (one-sample attack, 150 ms exponential decay, four hits); transient = output peak over input
peak in the first 10 ms, sustain = output over input RMS at 80–250 ms, each position against the unit's neutral run. Gates: a
sine ramping −70 → −6 dBFS and back for open / close level, hysteresis and range; a burst for attack / hold / release.

**Findings.** 1. Smack Attack's Attack is exactly linear (±100 → ±24 dB, 0.24 dB per unit). 2. G8's threshold and range
labels are exact to 0.0 dB and its hold within 4 %; C1 gate opens 0.9 dB above its label with 3 dB hysteresis. 3. Sustain
labels do not read on either hit length (MTransient ±24 reads ±2.9 on the short hit, ±0.9 on a 500 ms one): a sustain lane
acts against the unit's own envelope follower, and a fixed window reads a different part of it.

**Questions.** A transient profile as ±dB per position, or a feel ladder? How "sustain" is defined for a label comparison?
Gate timing to 1 dB / 20 dB (ours) or the vendor's full rise / fall?

## Multiband compressors

**Method.** The proposal's method with numbers: one tone per band at the band's centre from the default crossovers, that
band's threshold against it; the whole unit on a vocal-shaped multitone at five levels; the amount as the global control
where one exists, else a common dB offset on every band threshold. Each threshold is paired with the band it measurably
cuts (a flat multitone at its two ends), never by name order.

**Findings.** 1. The offset family works on C4 / LinMB / 354E — monotonic, the band balance kept, C4 at −24 dB reads 5.4
dB on the vocal signal while its bands read 5–8 dB on their own tones; 354E clamps at its −20 dB end. 2. OTT and Melda
need the global control's zero as the open reference and a SIGNED GR — both make gain at quiet levels (upward compression).
3. The pairing by measurement sorts C6's floating bands (Band 4 → 5, Band 5 → 6; Bands 1 and 6 cut nothing at their
defaults) and drops Melda's gate and processor-2 thresholds (they cut nothing); its processor-1 thresholds cut the same
region as the band's own.

**Questions.** Unchanged: the schema fields (`topology`, `offset_db`, per-band `in_at_gr`, the whole-unit figure's name)
and whether the server will write a common offset to N controls — plus a signed GR and the global zero as the reference.

## Across all nine

- Names propose, measurement decides: the control a mode uses is the one whose measured effect matches the role, with the
  reason on the record when a name was wrong (Saphira's band gains, bx_delay2500's Modulation Mix, TransX's amount hiding as
  "Range", Melda's 22 "thresholds"). Unnamed controls that show a signature are listed for a ruling, never written.
- Three Plugin Alliance licence shapes on this Mac: inert (passes untouched), silent (passes nothing), pass-through at every
  position (V76U73) — one of each wants a look in Logic.
- Every mode is a prototype: a record on disk, a proposal doc, no change to the compressor path Sean's follow-up runs.

## Appendix — controls the measurement found without a name (per category, one line each; Kathy's ruling 3, 5 Oct)

From the 5 Oct rehearsal batch (two products per category, the current rule) — `tools/ejmap/cert-traces/2026-10-05-phaseb/logs/`.
"Unnamed" means: the control's name nominated it for nothing, its measured two ends show the role's signature. None is written
to a profile; each is here for a ruling.

- **Gain / output:** none (mpressor 5, SBC 8 probed; nothing showed a gain's signature).
- **Compressor timing:** not probed (a burst pair per control costs ~12 s; the names nominate).
- **Limiters:** bx_limiter True Peak **Gain** — the output peak moves 5.95 dB and holds against a 6 dB drive change (a ceiling by behaviour).
- **EQs:** bx_digital V3 **Input Gain** and **Output Gain** (every tone +12.04 dB: a level that the grid guard did not catch here — open), **High-pass 1 Frequency** / **Low-pass 1 Frequency** (−61 dB at a tone: a filter corner read as a band's gain), **High-pass 1 Slope** (2.9 dB), **Bass Shift 1 Gain** (20.4), **Presence Shift 1 Gain** (16.4), **Dynamic EQ 1 Range** (24.1); AMEK EQ 200 **LP Freq 1 / 2**, **HP Freq 1 / 2** (−105..−121 dB: corners, not bands). Frequency and Q signatures are not probed for unnamed controls (they need a boosted band).
- **De-essers:** Lindell 902 **Mix** (GR 6.0 dB between its ends), **In Gain** (20.0), **Out Gain** (20.0) — a gain in front of the detector reads as a threshold; the threshold signature alone cannot tell them apart.
- **Saturation:** J37 **Input Level** (THD −87 → −10 dB: a drive by measurement), **Output Level** (THD −10 → −88: the output's level, read as THD at its silent end — the guard says drive), **WOW Rate / WOW Depth / Flutter Depth** — "modulation, unnamed" (energy beside the fundamental rises 12 dB over the harmonics'); NEOLD BIG AL none.
- **Reverbs:** Abbey Road Plates **Damper** (RT60 0.77 → 3.36 s, ×4.4: a decay); ValhallaVintageVerb **Size** (×1.7), **BassMult** (×1.8), **BassXover** (×2.6), **HighFreq** (×2.3, and the wet/dry ratio 18.5 dB), **EarlyDiffusion** (×1.5, onset 41 → 25 ms), **LateDiffusion** (×2.1), **ColorMode** (×1.7), **HighCut** (wet/dry 20 dB), **Attack** (onset 25 → 70 ms, wet/dry −14 dB).
- **Delays:** bx_delay2500 **Time R** (onset 14.5 → 2475 ms), **Feedback R** (1 → 32 repeats), **Feedback Low Pass / Feedback Hi Pass** — "tone (feedback path), unnamed" (the onset holds, the first repeat moves 16 / 10 dB), **Transient Shaping Threshold** (the fall per repeat −31 → −4.5 dB); H-Delay **Depth** (onset 375 → 456 ms: modulation delay read as time), **HiPass / LoPass / Output** — "tone (feedback path)" (the onset holds, the first repeat moves 30 / 41 / 36 dB).
- **Transient shapers:** none (Smack Attack 2, Transient Master 14 probed).
- **Gates:** Unfiltered Audio G8 **Dry/Wet** (the closed level moves 80 dB with the open level put: a range by behaviour); C1 gate none.
- **Multiband:** not probed (the pairing response per nominee is the measurement step).

Two reading limits are visible in the list and stay said rather than hidden: a filter corner swept across a tone reads as that
tone's "gain" (the EQ corners above), and a level in front of a detector reads as its threshold (the 902's gains).
