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
