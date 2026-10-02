# 2026-10-02 batch 10: the first live profile sweeps (07:46-08:34, mains from 07:46, iLok 0x00130000 / 2)

`--cert-sweep --profile` (COMP_PROFILE_SPEC v1.4: 31 levels -60..0 peak ascending per fresh
process, 2.5 s hold / 300 ms read, quiet reference on every product, the hold-doubled repeat
`r2.`), then per record `--cert-detector` -> `--export-profile` -> `--cert-tone-check`.
Three sub-directories, in the order they happened:

- `first-run-fixed-reference/` - the seven-product pile (`batch10.log`, `after_prof.log`) with the
  binary as armed on 1 Oct: the quiet reference was the fixed pair -54/-48. CL 1B certified but its
  reference failed its own 6 dB check on positions 6-15 (the plugin compresses at -48 there), so
  3 of 16 positions reached 1 dB and the export refused (needs 9). The other six: CLA-76 (s) and
  CLA-2A (s) certified (15/16 and 16/16 quiet checks; not yet through the detector), NEOLD U2A
  FLAT with the neutral set written (mix_wet [2], drive_cleanest [3]), VComp (s) FLAT with no
  neutral set, NEOLD V76U73 and Mike-E Comp refused at plan (no threshold role).
- `ladder-run-1/` - CL 1B again (08:17) with the reference ladder (-54/-48, -66/-60, -78/-72,
  -90/-84; loudest passing rung per position): 16 of 16 referenced, 10 below the first rung.
  `raw/clampfree_*.txt` are the two one-position measurements beside the tone check (the
  clamp-free interpolated pick, norm 0.2077 -> 1.58 dB, and the midpoint 0.2333 -> 2.37 dB).
- `final/` - CL 1B (08:31) with the shipped binary (manufacturer stamped, one-position derive and
  reference-less merge fixed): THE RECORD in `cert-fixtures/profiles/`, its detector processes
  (`*.detector.sine.1.txt`, `*.detector.twotone.1.txt`: 2 dB at -10.9 / -12.1, f = 0.40) and the
  tone check (`Tube-Tech_CL_1B_2.5.62.tonecheck.1.txt`: 1.33 dB at the rule's pick, FAIL).

Every reading on both CL 1B ladder runs agrees: same ten positions descend to the same rungs, the
same in_at_gr to 0.1 dB, point_error_db 0.1 on both. Paths are redacted (`~`).
