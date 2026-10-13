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

| row | traces in the 30 min | s / trace | candidates started | whole row (est.) |
|---|---|---|---|---|
| L360 (6->6) | 104 | 17.3 | 3 of 6 | ~1.0 h |
| L360 (5->5) | 128 | 14.1 | 3 of 5 | ~0.8 h |
| Spherix Limiter (10->10) | 69 | 26.1 | 2 of 10 | ~2.9 h |
| Spherix Limiter (12->12) | 57 | 31.6 | 1 of 12 | ~4.2 h |
| MLimiterMB | 198 | 9.1 | 6 of 7 | ~0.6 h |

For comparison, the 34 ok rows run at a median of 5.9 s per trace; the longest took 1292 s.

## 4. Publish v2
`check_v2.py` produced the first result (dbx-160 (s) only; material counted as a hold).

**Repackaged (follow-up): v2 uses v1's rule.** A product is held only for a frequency flag, an output-level (make-up) flag, or a missing accuracy record. A material flag alone, or a combined flag whose timing positions v1 already refuses, is not a hold.
- **`EJMAP_PUBLISH_COMPRESSORS_v2.zip`:** dbx-160 (s), Mixland Vac Attack, VBC FG-Red, DPR-402 (s).
- **DPR-402 (s):** its combined +1.17 dB comes from attack '100 msec' (-2.27) and release '5 msec' (-1.27), positions v1 refuses, as for (m). Its output net of GR is +0.04 dB.
- **Held:**
  - Auto-Tune Vocal Compressor: its combined pass composed nothing because its gain-cal / timing timed out.
  - VBC FG-Grey / FG-MU / Rack: the accuracy passes never ran on them.

`PUBLISH_LIST_v2.txt` gives each reason.

## 5. The cut: df695ba1 (`~/Desktop/ejmap-dist-13oct-df695ba1`)
- **Packaged:** built and signed (app and probe both team 8BT5F9B887); no quarantine flag (`xattr -lr`).
- **Preflight: REFUSED, only on this Mac's power** ("on battery: the Mac can sleep", the b287aef3 sleep guard). The sleep check returns before the quarantine line is printed, so that line has not been seen on this dist. **Re-run it on AC power:** `~/Desktop/ejmap-dist-13oct-df695ba1/ejmap.app/Contents/MacOS/ejmap --cert-preflight`.
- **Equality** (on the packaged binary, the same functions as `--cut`; `equality_packaged_df695ba1.txt`): every difference is named and 0 are unexplained.
  - compressor sets: 5 profiles not_written (d26db00f); AMEK, DSM V3, Millennia, SPL IRON and bx_opto in 'current', bx_opto in the other three;
  - drafts: 1307 identical.
- **Next night on this build (dry-run):** followup (the 86 checks re-run, about 40 min) and drafts.

## 6. The re-cut: b7aa304c (`~/Desktop/ejmap-dist-13oct-b7aa304c`; supersedes df695ba1)
It carries the follow-up's items 2, 3 and 5:
- the candidate guard;
- the accuracy-gap steps and the timing guard;
- `licensed_now` and `limiter_comp_unfinished`;
- the morning report's own-traces fix.

`morning.py --folder cert 5 --cut` ran end to end on AC (`morning_cut_b7aa304c.txt`):
- **Packaged and signed:** team 8BT5F9B887.
- **Preflight exit 0** (`preflight_b7aa304c_ac.txt`): sleep ok (AC, sleep 0), quarantine ok (the app, its executable and the probe), and `xattr -lr` shows no flag.
- **Equality vs b287aef3:** every difference is named and 0 are unexplained.
  - compressor sets: the same 5 `not_written` profiles (d26db00f) and nothing else; nothing else in the follow-up changes derive-only output;
  - drafts: 1307 identical.
- **Resume dry-run:** the night line below.

### Sean's next line (on b7aa304c)
```
caffeinate -dimsu "$BIN" --run-all --jobs 2 --steps preflight,followup,licensed_now,accuracy_gaps_drafts,accuracy_gaps,limiter_comp_unfinished,drafts --until 07:00
```

| step | what | serial | at --jobs 2 |
|---|---|---|---|
| followup | the 86 tone checks re-checked (the 3 fixed: AMEK, SPL IRON, MAGNUM-K) | 0:40 | 0:40 |
| licensed_now | the licence file's refusals it now admits (the UAD EQs once their lines are in) | 0:30 | 0 until the file changes |
| accuracy_gaps_drafts | Auto-Tune VC gain-cal + timing | 1:15 | ~1:00 |
| accuracy_gaps | combined 4, material 3, frequency 3 (Auto-Tune VC, the 3 VBCs) | 0:15 | ~0:08 |
| limiter_comp_unfinished | L360 6->6 / 5->5, Spherix 10->10 / 12->12, MLimiterMB | 12:50 | ~6:40 (Spherix 12->12 alone ~5:20) |
| **total** | | **15:30** | **~8.5 h** |

G8 is not in the line. b287aef3 filed it ok on 13 Oct with its crash position recorded; the morning report's flag came from a 6 Oct trace of an older run.
