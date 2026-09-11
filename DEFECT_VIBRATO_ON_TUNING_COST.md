# DEFECT: ignore-vibrato-ON costs 9 points of tuning on identical material

**Filed:** 2026-09-05, per ruling ("write this down now while it's cheap,
but don't build for it"). Sibling context: `DEFECT_GRAIN_EPOCH_UNITY.md`,
`tools/pitch_span_census`.

## The measurement (standing NEW reference set, 13.25 s, D minor)

Same take, same engine, only the switch differs:

    ignore_vibrato OFF:  same-semitone 95.4%   improve-rate 58.7%   (matches Antares 95.8 / 60.0)
    ignore_vibrato ON:   same-semitone 86.2%   improve-rate 51.4%

The structural slow-shift rebuild (pole-50 on the slow component, fast
term bypassing the pole) helped but did not close it. Every tuning metric
degrades with the switch on; it is currently the default.

## The hypothesis to test FIRST when this is picked up

The span census measured half of all voiced-span boundaries as tracking
blinks inside continuous notes (33 of 66 gaps on this take; one 1.44 s
note in eight spans). If span resumes also reset the corrector's
vibrato-phase tracking — the state that ignore-vibrato uses to separate
wobble from note — then the 9-point gap may be the SAME fragmentation
defect wearing a different coat: each mid-note resume restarts the
vibrato estimate from nothing, and targeting decisions made during the
re-estimation window are the wrong-semitone votes. A fragmentation fix
(or a corrector-side hold of vibrato state across sub-100 ms gaps) may
move this metric for free. Measure the vib-on tuning numbers after any
fragmentation change lands, BEFORE building anything vibrato-specific.

## The open product question (the reviewer's, not settled here)

Whether ignore_vibrato should remain the default while it measurably
degrades every tuning metric on reference material.

## MEASURED 2026-08-30: confirmed — the onset-seeding variant

The fragmentation-coat hypothesis is CONFIRMED in refined form. Short
blinks are innocent (gaps < kGapIsNoteChangeMs = 200 ms hold state by
design). The mechanism is the RE-SEED: at every voicing resume after a
>=200 ms gap and at every note change, `haveSlow_` drops and
`slowCents_` — the slow-smoothed track that ignore-vibrato TARGETS from
— is re-seeded from ONE instantaneous sample (`slowCents_ = inCents`),
which under vibrato can sit a full swing off the note's mean. The
140 ms smoother (kVibratoSmoothMs) then takes ~300 ms to converge, and
targeting votes from the biased value throughout.

The measurement (tools/pitch_vibvote, sourceNEW hard renders vs
antaresNEW, 68 re-seed events):

    vib ON:  wrong-semitone 11.7% within 300 ms of a re-seed
             vs 1.2% elsewhere — a 10x concentration;
             98% of all wrong frames live in re-seed windows.
    vib OFF: 4.3% vs 1.5% (ordinary onset transients; immune to the
             mechanism because targeting never consults slowCents_).

The entire ~9-point same-semitone gap is onset seeding; MID-NOTE,
vib-on targeting is as accurate as vib-off (1.2 vs 1.5%).

## Fix candidates (for ruling — none built)

(a) Onset-adaptive smoothing: seed as today but start kVibratoSmoothMs
    short (~30 ms) and relax to 140 over the first few hundred ms, so
    the slow track converges before votes matter.
(b) Provisional targeting: until the slow track has ~a half vibrato
    period of history, target the way vib-off does (fast f0), then
    hand over.
Both are corrector-local; neither touches the engine or the contract.

## RESOLVED 2026-08-30 — fix (d) shipped; residual UNDERSTOOD AND DECLINED

Fix (d), provisional selection on pending evidence, shipped in commit
5499adc (window wrong-rate 11.7 -> 7.0%, same-semitone 90.5 -> 94.6,
vibrato retention bit-identical, confirmed on source4: 6.5 -> 3.1%).

**The residual is filed, not open**: 7.0% in-window against vib-off's
4.3%, cause named and measured — ~35 ms tracker latency plus pending-
formation time, the interval BEFORE the note-change detector can hold a
pending at all. Closing it requires touching detection, which has been
deliberately kept honest throughout and carries its own cautionary
history (the F0JumpGate rounds: persistence-holds at the detection
layer injected excursions). Ruled 2026-08-30: understood-and-declined.
Do not reopen without new evidence about detection itself.

**Side-settlement**: (d) repaid the bleed gate's accepted natural cost
— natural vib-on rough spans 59 -> 56, back at the pre-gate number —
so the price recorded in PITCH_P0_VALIDATION.md §17.1 is now SETTLED
rather than outstanding.

## The finding of the boundary-suspension rounds (31 Aug 2026, recorded verbatim per ruling)

"The confirm window is the only true discriminator, so only detection can
move the trade-off — everything else redistributes it."

Context for whoever picks this up: the pending stream is settings-invariant
(detection reads only inCents vs the note anchor; no preset touches it) and
depth-blind (pre-pending vibrato depth distributions for confirmed vs
reverted pendings overlap almost completely on both reference takes). Any
policy that acts during the confirm window misfires on the ~25% revert
fraction. The applied-shift gate (envExp 5) sidesteps rather than solves
this: it conditions on how much the misfire would COST, not on whether the
pending is real. Moving the discriminator itself means touching detection —
a separately-ruled decision with its own cautionary history.

## CLOSED 2026-08-31 — envExp 5 shipped; env3 filed available-but-rejected

The note-boundary snap fix shipped as the applied-shift-gated release
(envExp 5 default; PITCH_P0_VALIDATION.md §17.5 item 4 for the constant's
regime). Natural bit-identical (unique among all candidates), hard better
on 6/8 columns, tau-400 rough spans 97 -> 76, hard-match clicks 2 -> 0,
ear neutral-or-better across two blind rounds.

**env3 (slow-track conjunction, TH=20): AVAILABLE BUT REJECTED.** It is
the only arm whose tau-400 benefit was ear-confirmed unprompted ("way
smoother", round 1) and it collapses the snap further (12.7c vs 26.6c) -
but it moved natural both times it was measured (release: 56->65 spans;
TH=20: 56->57 + same-semitone 98.4->97.5), and natural untouched was the
binding requirement. Kept debug-gated as envExp 3 with its numbers.

**Ear-calibration datum (record per ruling):** in round 2 Sean identified
a bit-identical pair as identical (nat120, byte-compared). His round-1
tau-400 detection ("way smoother") is therefore validated as a real
detection, and his round-2 miss of env5-vs-shipped at tau 400 is a real
threshold effect (the fix difference swims in 400ms's own scoop), not
noise in the listener. Weigh ear verdicts accordingly.

**Cross-reference (2 Sep 2026, onset pass round 4):** the independent
container measurement of Sean's 29 Aug bounces shows ignore-vib OFF tuning
better than ON (onset p75 12.77c vs 18.71c; this machine's rulers agree:
sustain jitter 0.97 vs 1.61, all-voiced <5c 48.9% vs 45.0%). Those bounces
PREDATE the (d) fix this file ships, so the data is consistent with the
defect as it stood and says nothing about the current engine. Filed for the
record; not re-opened.

**Cross-reference (2 Sep 2026, the waveform pivot):** on the NEW take,
ignore-vib ON costs GLITCH EVENTS as well as tuning - current engine
tau6: 7 events vs OFF's 4 (29 Aug bounces: 11 vs 3; container: 30 vs
12). If both costs stand after Sean's ear pass, this filing's
"understood and declined" status needs revisiting. Note the OLD take
inverts (ON 0 events, OFF 4) - the cost is material-dependent.

## RE-OPENED 2026-09-03 - standalone, unbundled from the seam-attack work

The 2 Sep cross-references above set a condition: "if both costs stand
after Sean's ear pass, this filing's 'understood and declined' status
needs revisiting." The ear pass has now happened (seam attack ear gate, ON
intro region, 3 Sep) and both costs stand. Re-opened.

What seam attack (default 60ms, ONSET_PASS_RECORD.md round 16) did and
did not do for ignore-vibrato ON, on the NEW take:

    word-start glitch events   OFF: 100% -> 0%  (zero events)
                               ON:   71% -> 20% (1 of 5 remains)
    paired sustain delta med   ON  +0.032c  OFF -0.009c
    residual ON population     aperiodic 15-25ms mid-note re-entries in
                               the turbulent intro, re-zeroing a 30c+
                               standing correction (v1 dissection)

So ON still carries an EVENT cost that OFF does not, on top of the tuning
cost this file has measured since it was filed (same-semitone 86.2 -> 94.6
after fix (d), never the 97-98 of OFF; 29 Aug container: onset p75 18.71c
vs OFF's 12.77c). The seam-attack default was flipped on the strength of
OFF's zero and ON's ear pass; it is NOT a fix for this defect and must not
be cited as one.

Why "unbundled": the seam work's bar was word-start continuity. The ON
residual has a different mechanism (re-seed of slowCents_ from one sample
at voicing resume, plus the ~35ms tracker-latency window the (d) fix could
not reach) and a different discriminator problem (its re-entries are
genuinely aperiodic and pass every seam test). Bundling it into the seam
pass would have meant renegotiating a bar that already passed. It gets its
own ruling and its own bar.

Open for ruling (the same two questions as before, plus one):
  1. Should ignore_vibrato remain the DEFAULT while it measurably costs
     both tuning and glitch events on the reference take? (Material-
     dependent: the OLD take inverts - ON 0 events, OFF 4.)
  2. Is the detection-layer residual (the 7.0% vs 4.3% in-window rate)
     worth touching now, given the F0JumpGate cautionary history?
  3. NEW: does the ON residual event (1 of 5, intro region) survive on
     broader material, or is it this take's turbulent intro alone?
     Measure alongside the seam-attack broader validation that is owed.
