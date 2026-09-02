# DEFECT: grain-path epoch instability on real glottal pulses — breaks at ratio 1.0

**Filed:** 2026-09-05, from the constant-shift probe run of the roughness
investigation (`tools/pitch_constshift_probe`, source4). Ruled: recorded,
not chased.

## The measurement

Real voice (source4, low-male, creaky patches) through PsolaEngine with the
splice band force-disabled (`debugDisableSplice`) — the GRAIN path — at a
constant shift of **0 cents, ratio exactly 1.0**, retune absent entirely:

    grain path @ 0c:  141 break-windows (cycle similarity < 0.5),
                      15 with successive periods INVERTING (similarity < 0),
                      median similarity 0.9496 vs the source's own 0.9627
    splice path @ 0c: 0 breaks, 0 inversions, bit-clean (identity)
    ideal control:    0 breaks (the input is its own ideal at r=1)

Only 15 of the 141 are explained by drift-displaced source transients; the
rest are the synthesis itself. At unity, TD-PSOLA should approach a copy;
epoch placement jittering or doubling on real glottal pulses (creak,
period-doubling patches — exactly where the detector's period estimate is
least stable) makes grains overlap incoherently instead.

## Scope — why this is not the field defect

**Out-of-band only.** In-band (|shift| <= 2.5 st) the shipped preserve path
rides the splice-resampler with `methodMix_ = 0`: the grain output is fully
replaced, and the per-sample author trace (`PsolaEngine::debugSpliceTrace`)
confirms zero grain leakage across every in-band run. Correction-scale
shifts never touch this path.

## When it will matter

The first time anyone shifts beyond the splice band — formant_mode `shift`
(which never splices; its envelope warp needs the LPC grain path), or any
correction/effect pushed past 2.5 semitones, where `methodMix_` crossfades
to 1 and the grain path IS the output. On creaky or fry-heavy material it
will carry roughly this take's density of breaks before any shift error is
even added.

## The investigation trail

Probe variants and numbers: `tools/pitch_constshift_probe/main.cpp` (the
`grain` variant rows). Reproduce with:

    g++ -std=c++17 -O2 -ISource tools/pitch_constshift_probe/main.cpp -o probe
    ./probe "/Users/SeanD/Music/Logic/test/Audio Files/source4.wav"

The fix direction, when chased: epoch stability on real glottal pulses
(does `nextEpoch`'s 0.7T–1.3T peak-pick hold the same phase point through
creak?), before anything about grain gains or windows.

## RE-OPENED 2 Sep 2026 (the onset-shakiness pass) — and the ordered question answered in writing

**Q: did "out-of-band only" exclude low-confidence or onset frames?**
**A: YES.** The 141-breaks census (tools/pitch_constshift_probe) gated
every counted window on source periodicity >= 0.5 — which excludes
precisely the aperiodic onset frames under investigation. The dismissal
AS IT APPLIES TO ONSETS was therefore void as written. (The
"out-of-band only" routing claim itself — methodMix 0 in-band — remains
true and verified by the author trace.)

**The onset-windowed re-measure (tools/pitch_onset_probe, alto_tenor,
ratio exactly 1.0, grain path forced):** onset jitter 5.47 c/hop vs the
SOURCE's own 5.22; sustain 1.27 vs source 1.41. At onset windows the
grain path measures within 5% of the raw take. The epoch path
contributes ~nothing to onset shakiness on this metric; the 141-break
figure stands as filed for sustained out-of-band material only.

## VOID - CLOSED WITH A PITCH METRIC (2 Sep 2026, the waveform pivot)

The 2 Sep onset-windowed clearance ("grain onset jitter 5.47 vs source
5.22, within 5%") compared JITTER - a pitch aggregate - against a
filing whose subject is BREAKS AND INVERSIONS, which the research doc
classes as click-class undefined behaviour. A jitter metric cannot see
a click; twelve events cannot move a median. The clearance is VOID.

Event-level re-measure (tools/pitch_glitch_events, LTP-residual excess
> 1.5 vs source, grain path forced at unity, current engine):
  sourceNEW (alto): **30 events** (10 within 150ms of word starts) -
    against 4-7 for the full chain on the same take.
  dry.wav (low_male): 6 events.
  source4 (the census material): 16 events.
The grain path produces the event class in quantity. Timestamp
cross-check against the original census's 15 inversions is PARTIAL:
pitch_constshift_probe prints only per-variant worst windows (4.24 /
5.46 / 8.98 / 9.53s on source4), which do not line up 1:1 with the
event list; a full inversion-timestamp dump needs a probe extension.
The VOID stands on the instrument-class argument regardless. Scope
note unchanged: in-band production audio never routes the grain path
(methodMix 0, author-trace verified); this filing matters out-of-band
and for formant_mode shift.
