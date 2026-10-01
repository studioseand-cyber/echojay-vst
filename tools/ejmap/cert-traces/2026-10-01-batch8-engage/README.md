# 2026-10-01 batch 8: engage detection on the six pass-through products (14:3x-15:39, mains, iLok 0x00130000 / 1)

The two-sweep test, live: each threshold candidate's first sweep read pass-through at the
instantiate defaults; each engage candidate was then tried in a quick three-position probe with
that one write (`eq<idx>.` traces); the first showing gain reduction was VERIFIED (GR with the
write, pass-through without) and the full sweep re-ran with it (`e<idx>.` traces). Fixtures
replace the six in the store; `thresholdSweep.engageWrites` carries writes / tried / found.

- **MaxxVolume (m/s)**: Low Level Thresh and High Level Thresh CERTIFY with `Low Level Thresh On ->
  1` and `High Level Thresh On -> 1` (first candidate tried each time: the affinity ordering put
  the switch sharing the threshold's words first). The two "...On" switches themselves, swept as
  thresholds, stay pass-through (4 tried) - correct: they are the engage, not the amount.
- **EMO-D5 (m/s)**: Gate Thresh, Comp Thresh, Limiter Thresh CERTIFY with `Gate On`, `Comp On`,
  `Limiter On` (1 tried each). Leveller Thresh: `Leveller On` verified (GR appeared) but the full
  sweep is unreadable. DeEsser Thresh: NOT FOUND after 14 candidates - a de-esser's band does not
  cover a 997 Hz tone, so no switch could make it respond (tone choice, cause 4; not an engage
  failure).
- **dbx-160 (s)**: NOT FOUND, 2 candidates (SC-HP L/R; Noise, Comp Mode, Monitor excluded or not
  switch-shaped). Stays pass-through with the tried list. That is the real answer for this harness:
  nothing switch-shaped engages it.
- **DynOne3**: NOT FOUND on all 15 thresholds, 1 candidate each ("In Gain", name-matched "in").
  Stays pass-through. No switch-shaped control exists; what enables it is not a switch.

Count: 4 of 6 products became measurable (10 candidates certified, 2 unreadable); 2 stay
pass-through with every candidate tried and recorded.
