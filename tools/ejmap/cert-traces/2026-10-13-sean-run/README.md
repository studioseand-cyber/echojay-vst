# Sean's b287aef3 run (cert 5, 13 Oct)

Sean's full cert folder `~/Downloads/cert 5` (identical to `ejmap_cert_2026-10-13.zip`: 100,335 files, the same sizes). Built by b287aef3; all 9 run-all steps done in about 13 h. Read only; every derivation ran on a scratch copy.

## 1. The morning report (no cut)
`morning_folder_cert5_precut_98c96e4f.txt`: the run-all steps, the 86 tone checks (83 pass g 2), rows by outcome, and the equality against b287aef3.
- Before the item-2 fix (62608ad7): 0 differences.
- After it (98c96e4f): the five `not_written` profiles below, 0 unexplained.

## 2. The three failing tone checks
Each was diagnosed from Sean's traces and then re-run live here, with the probe and the tone check's own arguments.

**AMEK Mastering Compressor** (1.37 dB vs 2.0; per channel 0.31 / 2.79)
- **Not the cause:** the linked twin. 03023e41 took the twin out, and the split stayed.
- **The cause:** the tone check writes 113 neutrals, while the sweep writes 2. Bisecting live found the culprit: writing 'M' [57] at its own instantiate value 0.0 stops Param Link copying Threshold 1's make-up onto channel 2 (quiet: L +4.47, R +0.00).
- **After the fix:** without the 59 unnamed / shared-letter controls, GR is 2.01 and the channels agree within 0.04.

**SPL IRON** (unreadable; it passed 1.99 on 5ab8352b)
- **The cause:** four unnamed controls [40..43] with empty labels, whose values the unit moves itself. Since 96e93af6, [40] and [41] are written at the sweep's 0.084. After that [42] never lands, so the probe refuses.
- **After the fix:** without them, GR is 2.02.

**Maag MAGNUM-K** (0.49 vs 1.0)
- **The measurement is right:** the sweep's own traces read about 0.5 dB there.
- **The cause:** at L −18.40 the 0.95 detent lies under its 1 dB point (−15.31), and the reverse read clamped to 1.0.
- **After the fix:** there is no expectation at that level, so the L rule tests at −10.61 (the detent's 2 dB point). Live: 1.99 (channels 1.94 / 2.04).

**The fixes** (d26db00f; pins TC-1 / TC-1b / TC-2, 5 mutants red):
- **Neutral writes:** a control whose name is empty or shared, and whose label is empty or only echoes its value, is never a neutral. It is listed in `not_written`.
- **Expectation:** a stepped pick under its detent's shallowest point carries `expectationBelowCurve`, and the L rule moves on.

**Derive-only effect** (5 profiles; nothing named or labelled dropped):

| profile | neutral before → after |
|---|---|
| AMEK | 113 → 54 |
| DSM V3 | 40 → 32 |
| Millennia TCL-2 | 18 → 16 |
| SPL IRON | 36 → 32 |
| bx_opto | 8 → 7 |

bx_opto and DSM V3 are in the published v1 set.

## 3. limiter_comp: the 5 timed_out
The hang guard is **too short**: they are slow, not stuck. In every row, every probe process ran to its own end marker (the only unended files are the parameter-list reads, which never carry one). The row logs show candidate after candidate starting.

The causes:
- **One threshold per channel:** each channel's threshold is its own candidate (L360 6→6: 6, L360 5→5: 5, Spherix 10→10: 10, Spherix 12→12: 12, MLimiterMB: 7 incl. the 6 bands).
- **Slow processes:** 6 to 12 channels per process.
- **Running in pairs:** at `--jobs 2` the twins ran side by side.

| row | traces in the 30 min | s / trace | candidates started | to finish (est.) |
|---|---|---|---|---|
| L360 (6->6) | 104 | 17.3 | 3 of 6 | ~1.0 h |
| L360 (5->5) | 128 | 14.1 | 3 of 5 | ~0.8 h |
| Spherix Limiter (10->10) | 69 | 26.1 | 2 of 10 | ~2.9 h |
| Spherix Limiter (12->12) | 57 | 31.6 | 1 of 12 | ~4.2 h |
| MLimiterMB | 198 | 9.1 | 6 of 7 | ~0.6 h |

For comparison, the 34 ok rows run at a median of 5.9 s per trace; the longest took 1292 s.

## 4. Publish v2
`check_v2.py` produced the result (`v2_check.json`, `PUBLISH_LIST_v2.txt`), packaged as `~/Desktop/EJMAP_PUBLISH_COMPRESSORS_v2.zip`. **Clean: dbx-160 (s) only.** The other seven are held, each with its reason in the list.
