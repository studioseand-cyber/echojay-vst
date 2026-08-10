# DEFECT: dial-time readback tolerance is ladder-blind — correct writes to stepped parameters are reverted

**Filed:** 2026-08-02, from the ejmap M9 review (decided: filed independently
of M9). Sibling filing: `DEFECT_BRIDGED_READBACK.md` (the readback layer's
other measured failure).

## The live instance

A `q: 0.7` request on a plugin whose Q parameter is a stepped ladder
(… 0.6, 0.8 …). The request value sits at the exact midpoint. The plugin
rounds the write to 0.8 — correct behaviour for a stepped control. The
dial-time readback compares the read display against the REQUEST (0.7) with
an absolute tolerance; the difference beat the tolerance by 3e-8; a correct
write was reverted and reported as needing hand dialling.

## The defect class

The readback expectation is derived from the raw request instead of from what
the parameter can actually express. Any stepped or coarsely-quantised
parameter whose rung spacing exceeds the hand tolerance will fail readback on
requests that fall between rungs — deterministically, forever, on correct
writes. The failure mode is the worst kind: the verification step is the
thing doing the damage, and it reports the damage as diligence.

## The fix direction (consumer-side, v2)

The maps carry the ladder: anchors are `[value, normalised]` tables, and for
stepped parameters the anchors ARE the rungs. The expectation must be
computed through the same functions the dial path already uses
(`dominantMonotonicTable` + `interpolateAnchors` in
`Source/EchoJayParamApply.h`): the predicted landing for a between-rungs
request is the rung the write will land on, and the readback tolerance must
be derived from the map's own local rung spacing (e.g. half the distance to
the adjacent anchor), never an absolute epsilon. A request of 0.7 on a
0.6/0.8 ladder then predicts 0.8, reads 0.8, and passes — because it is
correct.

ejmap's M9 verdict rules adopt exactly this ladder rule for probe
predictions; this filing is the consumer-side (dial-time) counterpart, which
M9 cannot reach from the mapping tool.

## Status

Documented only; no code changed. Blast radius: every stepped parameter in
every served map dialled with a between-rungs request — the class is
deterministic, so every affected request fails every time.
