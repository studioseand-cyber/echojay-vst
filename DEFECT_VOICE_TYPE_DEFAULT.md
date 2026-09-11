# DEFECT (product): the alto_tenor default mismatches the most common male vocal range

**Filed:** 2026-09-01, per ruling — the default/auto question is decided
separately from any code fix; this file carries the measured evidence.

## The evidence

- The schema default for `voice_type` is **alto_tenor**; its own
  description warns "match it to the source - a wrong choice causes
  octave errors". Sean's session sat on the default with low-male
  material and had no way to know.
- Measured consequence chain (sourceNEW, retune 400): at alto_tenor the
  previous phrase's mis-detections drag the retune envelope off; a
  sub-200ms gap RESUMES that limbo position ("the note in progress");
  the new syllable holds ~-123c OFF-GRID for 230ms (5.20-5.43s), one of
  nine such divergences, eight negative. At low_male the same take, same
  settings: none. The offline instrument stack pinned low_male and
  could not see it (a full investigation round was lost to this).
- The editor now surfaces the mismatch (amber: "voice alto_tenor -
  range suggests low_male") — a readout, not a behaviour change.

## The open questions for the product ruling

1. Should the default change (to what — low_male biases the other way)?
2. Should voice_type auto-detect from running evidence (the readout's
   estimator already computes the suggestion)?
Neither decided here.
