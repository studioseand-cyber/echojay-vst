# 2026-09-30 batch 6: the quiet-level fallback, live on the two always-on products

AMEK Mastering Compressor (18:13-18:14) and OTT (18:10-18:11), re-swept from scratch against a store
without their fixtures, driver with the reading rules (tone guard, all-positions flat test,
constant-offset pass-through), the relative reference guards and the quiet-level fallback
(`q.c<idx>.*` traces are the fallback runs; `c<idx>.*` the first pass). Mains, lid open, iLok on
0x01120000 / 2 (neither product needs it).

- AMEK: both thresholds' soft ends spread 9.16 dB across the levels (127% of a 7.2 dB response);
  fell back; every position linear at -54/-48 (check 0.00); BOTH CERTIFY, higher_is_harder,
  identical curves (a channel pair). The 18:09 AMEK run in the same log is superseded: it stopped
  at "not across the sweep" because the sense was read at the loud level only - fixed (pin F3)
  before the 18:13 run.
- OTT: Thresh L flat; Thresh M and H fell back and the quiet check FAILED at most positions
  (-48 minus -54 reads about -4.5 dB, not -6): OTT compresses upward at the quiet levels as well.
  No level is linear at any setting. Unreadable, honestly; not a case for another mechanism.
