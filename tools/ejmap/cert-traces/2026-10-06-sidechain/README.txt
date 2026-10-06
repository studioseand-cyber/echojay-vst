SIDECHAIN POLICY EXPERIMENT (6 Oct 2026, Kathy's item 1 + Sean's amendment) - this Mac, Sean's tone-check traces as the
process (run4.py rebuilds the probe's --sweep arguments from a trace), one process per policy, the probe's new sidechain=
switch (default unchanged = unconnected):
  silent      every element connected and fed zeros (the pre-4 Oct state)
  unconnected the render callback removed from every element past the main one (the shipped policy)
  self        every element connected and fed the main input's own signal
  echojay     the FIRST extra element connected and fed the main signal, the rest unconnected (EchoJay since build 04e)

GR at the tone check's own pick, by policy (policies/<tag>.<policy>.txt):
  Zip (tone-check writes as Sean's run made them)        0.00 / 0.00 / 0.00 / 0.00   <- the ratio was written at 1:1 (see below)
  Zip with its ratio precondition restored (5 -> 0.4219) 1.98 / 1.98 / 1.98 / 1.98   (target 2.0: PASS under every policy)
  C1 comp-gate (s) [no sidechain bus declared]           0.69 x4   (Waves 12 here vs 15 on Sean's Mac: the pick differs; policy-independent)
  C1 comp-sc (s)   [no sidechain bus declared]           0.69 x4
  C1 comp (s)      silent 0.00 | unconnected 0.67 | self 0.67 | echojay 0.67   (keys from a CONNECTED sidechain, as on 4 Oct)
  EMO-D5 (s)       1.98 x4 at its own L      Lindell SBC 2.00 x4      elysia mpressor 2.01 x4 at its own L
  RCompressor (s)  not runnable here (Waves 12: "no parameter 8")
VBC FG-Red / FG-Grey / FG-MU / VBC Rack: not installed here (Slate, licence-bound) - not loaded.

VERDICT: the sidechain policy is NOT what changed. Every unit reads the same under unconnected, self and echojay; only
C1 comp (s) differs, and only under connected-silent (0). What failed Sean's eleven tone checks is in the WRITES: the
follow-up's re-derive rebuilt each record's plan without its preconditions (the ratio raise, make-up zero, mix at wet),
the export fell back to the instantiate norm, and the tone check wrote Zip's ratio back at 1:1 (and C1 comp-gate/-sc's,
FG-Red's, FG-Grey's). 49 records lost their writes; 7 of the 11 failures are exactly the ratio_raise products.

Sean's amendment 2: every product in his records declares AT MOST ONE extra input bus (61 compressors, 90 Phase B
products with exactly one; 0 with more) - "first self-keyed, rest unconnected" and "all self-keyed" are the same policy on
his whole catalogue, and both read as unconnected on every unit measured here.
