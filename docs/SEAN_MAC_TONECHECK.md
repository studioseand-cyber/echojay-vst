# The follow-up on Sean's Mac: deep points and the v1.7 tone checks, no sweeps (2 Oct 2026; L per level, licence skip and CL 1B import 3 Oct)

After the batch has run with the build from `36397676`, the deep points (4/5/6 dB, spec v1.7 §3) and
the amended pick and tone check (§6.4, §8) need a newer build — but **no plugin needs re-sweeping**:
the traces the batch kept in `~/Library/ejmap/cert` already hold every level at every position. This
mode re-derives each exported record from its traces, re-exports the profile with the deep points, and
loads each certified plugin only for the tone checks. It is resumable and uses the same window watch.

**The follow-up build is commit `17f28da4`** (branch `feat/ejmap-cert`; packaged by `docs/PACKAGING_EJMAP_APP.md`
step 1 with that commit checked out — NOT `36397676`, which is the batch build and stays as it is). It carries v1.8–v2.1 and the raw hold test:
targets 1..12 with the clamp to 30 and the saturation note, the reverse read, the depth-aware clamp, the vocal-anchored tone level, every deep null accounted for in `notes` (a list),
out-of-order deep points nulled before export, the shallow-break refusal, the known-licence skip and the §11 guard.
Rehearsed here as a packaged app (3 Oct, evening): pre-flight finds the probe beside the executable; CL 1B's dry run
reads −14.45 at every level 2, 4..12; the four rehearsal units re-check 40 of 40 levels PASS with no `--probe`.

## What Sean runs (after the follow-up `ejmap.app` is built from `17f28da4` and packaged the same way)

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
"$BIN" --cert-preflight
mkdir -p ~/Library/ejmap/cert                     # the folder exists after the batch; harmless, and tee needs it
caffeinate -i "$BIN" --cert-tonecheck-all 2>&1 | tee -a ~/Library/ejmap/cert/tonecheck.log
```

Per exported product it: re-derives the record from `cert/<identity>.sweep.processes.json` + `cert/raw/`
(keeping the detector, Rule 1's decision, the map state); re-exports `cert/profiles/<Product>.json`
with `in_at_gr_dbfs` 1–6 and `quality.deep_point_error_db`; runs the tone check at g = 2 with the v1.7
pick, then one check per deep level the profile carries (4, 5, 6), each to 0.5 dB; a failing deep level
is nulled across all positions (the profile stands); the result is embedded as `tone_check` with
`deep_levels[]` and `deep_levels_nulled[]`, and the row in `cert/outcomes.json` is rewritten.

**The test level per level (3 Oct).** Each level g (2, 4, 5, 6) is checked at the level the server would
ask at for the spec's example track, through this unit's detector: L_ref = −18.4 + f × (−6.2 + 18.4 −
3.01) with f = `detector_f` (an RMS unit −18.4, a peak unit −9.21, CL 1B −14.45); if the §6.4 pick at
L_ref fails its clamp (12 dB up to 3 dB, then 12 + 2 × (g − 3)), the nearest valid level among the
positions' `in_at_gr[g]` values is used instead. Every level records `L_ref_dbfs`, the L it was tested
at (`L_rms_dbfs`), the gap and the rule (`L_rule`). A level is
nulled across all positions only for one of two named reasons (`null_reason`): `failed_check_at_L` (the
GR missed by more than 0.5 dB at the recorded L) or `no_valid_L_clamp_geometry` (the unit's own 1→g
spacing is at least 12 dB at every position — `spacing_1_to_g_min_db` says how much — so no L inside
the clamp exists; the unit's property, not a failed check). On the four rehearsal units every level tests at
L_ref itself and passes (Lindell SBC's 6 dB included, under the v1.9 clamp).

- **Resumable:** run the same command again; a profile whose tone check already carries `spec: v1.7`,
  its deep levels and `L_ref_dbfs` is skipped; `--retry-licence` reaches only the `needs_licence` set (they
  have no result file).
- **Licence — nothing known to need one is loaded (3 Oct).** A product the scan stopped, or one with a
  `needs_licence` row in this folder, is not loaded at all (the row says *known from the scan* / *known
  from this folder's outcomes*); a window during a check makes the row `needs_licence` (the export
  stands). `--retry-licence` is the only way past, when the licence is back.
- **Section 11:** the installed version must be the record's; a record from another version (or an
  unknown version) is `needs_review` with the §11 reason and is never loaded.
- **No sweeps, nothing sent.** A record without traces (none expected from the batch) is tone-checked on
  its existing points and says so.

## Tube-Tech CL 1B

**If tonight's batch certifies CL 1B on Sean's Mac, his fresh record is the one that is used** — the
follow-up re-derives and tone-checks it like every other exported product, and nothing from this repo
is involved.

If it does not (the iLok was away at his scan, so CL 1B is `needs_licence`; or the sweep refused), the
committed traces from here can be brought into his tone-check folder — the record, its process list and
the 62 raw captures, 650 KB — and the follow-up then re-derives and tone-checks CL 1B on his Mac from
them, with the iLok present:

```
# on the building Mac, from the repo:
tools/ejmap/packaging/export_traces.sh AudioUnit_517f614e_2.5.62     tools/ejmap/cert-traces/2026-10-02-batch10-profile/refined ~/Desktop/cl1b-traces.zip
# on Sean's Mac, before the follow-up command (-n: never overwrites his own files):
cd ~/Library/ejmap && unzip -n ~/Desktop/cl1b-traces.zip
"$BIN" --cert-tonecheck-all --retry-licence 2>&1 | tee -a ~/Library/ejmap/cert/tonecheck.log
```

`--retry-licence` is needed only because his scan stopped CL 1B (a known licence stop is otherwise not
loaded); with the iLok in, the check runs. **The §11 guard is in the binary:** the traces are of CL 1B
**2.5.62**; if his installed CL 1B is any other version the row is `needs_review` with *installed
version X differs from the record's 2.5.62 (section 11)* and nothing is loaded — rehearsed here on an
edited record. If CL 1B is not installed at all, the log says it resolves to no component and the row is `needs_review`. The imported
record is keyed by its identity, so his own CL 1B record (if any) is never overwritten (`unzip -n`).

## About how long

Measured here on 2 Oct on four exported products (traces from the rehearsal): **17 s in all** —
re-derivation, export and four probe processes each (g = 2 plus 4/5/6; the probe renders offline, so a
nine-hold tone process is about a second). Call it **10–15 s per exported product** (ten tone processes each since v1.10): a run that
exported 60 products is about fifteen minutes. It can be stopped and resumed.

## What he zips back

Exactly as before: `cd ~/Library/ejmap && zip -rq ~/Desktop/ejmap-tonecheck-$(hostname -s)-$(date +%Y%m%d).zip cert`
— `cert/` only, never `config.json`. The profiles, their `.tonecheck.json`, the re-derived records and
`outcomes.json` are all inside.
