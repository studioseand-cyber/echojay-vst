# 2026-09-30 batch 5: candidate sweeps (multi-threshold products)

The 42-product pile from `pile.txt` (products whose roles pass named more than one
threshold candidate), run against an EMPTY store with `--cert-sweep` per product,
iLok on port 0x01130000 / 2, mains, lid open. Every candidate control is swept and
labelled; the fixtures carry `thresholdCandidates` + `thresholdReview`, never a
`thresholdSweep`.

STOPPED BY HAND at 10:26 on 30 Sep after product 26 of 42 (the Mac was needed).

- Completed: 26 (fixtures in `cert-fixtures/compressor-profiles/`, one processes.json each here).
- Interrupted mid-write: product 27, MDynamicsMB. One raw file landed in `raw/`
  (`AudioUnit_6151642a_14.16.0*`); no processes.json, no fixture. Its raw file is a
  partial trace and is NOT a result.
- Not started (16): MDynamicsMB, MDynamicsMBLarge, Millennia TCL-2, MSpectralDynamics,
  MSpectralDynamicsMini, MTurboCompMB, OTT, Pro Audio DSP DSM V3, PuigChild 670 (s),
  Shadow Hills Class A Mastering Comp, Shadow Hills Mastering Compressor, Solid Dynamics,
  SPL IRON, SSL G3 MultiBusComp, Unfiltered Audio Zip, Vertigo VSC-2.

Resume: re-run the pile with the store containing these 26 fixtures (or `--skip` them);
discovery skips a product that already has a fixture at the installed version.

`run.jsonl` is the driver's per-process log; `/Users/<name>` is redacted to `~` in
plugin logger lines. No analysis has been done on this batch.
