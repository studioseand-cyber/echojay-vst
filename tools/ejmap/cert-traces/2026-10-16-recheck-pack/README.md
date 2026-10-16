# Sean's re-check pack, 16 Oct (Kathy)

- **Pack:** `~/Desktop/SEAN_RECHECK_PACK_16oct/` and `.zip` (sha256 7258bebe48eadc56..., 22 MB).
  - **Contents:** ejmap.app (feat/ejmap-cert 26b3cbaf, Developer ID 8BT5F9B887, not notarised), recheck.sh (the one command), held_units.txt, RUNBOOK.md.
- **The units (46):**
  - the 38 checked on 5 Oct (`2026-10-10-publish/HELD_LIST.txt`);
  - AMEK Mastering Compressor, SPL IRON, Maag MAGNUM-K;
  - VBC FG-Grey / FG-MU / Rack, Auto-Tune Vocal Compressor;
  - plus Pro Audio DSP DSM V3, which is in READY_33 as v1 shipped it while b7aa304c re-exports it without its unnamed neutrals.
- **The line:**
  ```
  --run-all --jobs 2 --steps preflight,followup,accuracy_gaps_drafts,accuracy_gaps,drafts --only <each unit> --until 07:00 --out ~/Library/ejmap/cert
  ```
  - With `--only`, the follow-up gets `--slice`: only these units are re-derived, re-exported and tone-checked.
  - Every one of them is stale under the new build, so every one runs.
- **Equality** on the PACKAGED binary vs b287aef3 (cert 5 copy, derive-only, nothing loaded): `equality_and_dryrun_26b3cbaf.txt`.
  - Compressor sets: only the 5 / 1 / 1 / 1 `not_written` profiles (d26db00f, the b7aa304c change).
  - Drafts: 1307 identical, 0 unexplained.
- **Preflight** on the packaged app (Kathy's Mac): probe beside the executable, team 8BT5F9B887, quarantine ok.
  - REFUSED only on battery: this Mac was not on AC; Sean's preflight runs inside recheck.sh.
  - It wrote nothing under `~/Library/ejmap`.
- **Dry-run** (nothing loaded): 5 steps, ETA 2:10:10 at 2 jobs.
- **The guard:** no plugin was loaded on Kathy's Mac to make this pack (EjmapSelfGuard rule, addendum 19).
- **READY_33:** `~/Desktop/EJMAP_PUBLISH_COMPRESSORS_READY_33.zip` (sha256 47c953a7f82c1826...).
  - v1's 29 (all 8 proposed v1 holds held, Kathy 16 Oct) + v2's 4, files unmodified; one-page list inside and beside it.
- **Nothing was sent.**

## v1's rule on the 46, from cert 5 (16 Oct, Kathy: "run v1's rule on the 46 from cert 5")
- **Inputs:** check_v2.py (the v2 judge) over the 46 names, read only, b287aef3 tone checks. Raw output: `v1_rule_46_cert5_raw.txt`; classified: `v1_rule_46_cert5.json`.
- **The rule:**
  - HOLD: a miss, missing level or nulled level at g 2/4/5/6; a missing F/M/C record; frequency_dependent; an output-level flag (make-up clamped, or output net of GR > 0.5 dB).
  - NOT a hold: material alone, or a level above 6 dB nulled.
  - COMBINED: a combined GR miss only - a hold unless it comes from timing positions v1 already refuses; not judged yet.
- **Result:** 12 CLEAR, 4 COMBINED, 30 HELD.
  - CLEAR: bx_opto, elysia alpha mix, MaxxVolume m/s, OneKnob Pressure m/s, Renaissance Axx m/s, RVox m/s, SSL Native X-ValveComp v6, DSM V3.
  - COMBINED: elysia alpha master, Millennia TCL-2, MixWave DW Fearn VT-7, SSL Native X-Comp v6.
- **Caveat:** bx_opto, Millennia TCL-2 and DSM V3 are re-exported by b7aa304c / 26b3cbaf without their unnamed neutrals (not_written). The file to publish is the re-check's, not cert 5's.
