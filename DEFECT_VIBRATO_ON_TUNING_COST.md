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
