# The follow-up on Sean's Mac: deep points and the v1.7 tone checks, no sweeps (2 Oct 2026)

After the batch has run with the build from `36397676`, the deep points (4/5/6 dB, spec v1.7 §3) and
the amended pick and tone check (§6.4, §8) need a newer build — but **no plugin needs re-sweeping**:
the traces the batch kept in `~/Library/ejmap/cert` already hold every level at every position. This
mode re-derives each exported record from its traces, re-exports the profile with the deep points, and
loads each certified plugin only for the tone checks. It is resumable and uses the same window watch.

## What Sean runs (after a newer `ejmap.app` is built and packaged the same way)

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
"$BIN" --cert-preflight
caffeinate -i "$BIN" --cert-tonecheck-all 2>&1 | tee -a ~/Library/ejmap/cert/tonecheck.log
```

Per exported product it: re-derives the record from `cert/<identity>.sweep.processes.json` + `cert/raw/`
(keeping the detector, Rule 1's decision, the map state); re-exports `cert/profiles/<Product>.json`
with `in_at_gr_dbfs` 1–6 and `quality.deep_point_error_db`; runs the tone check at g = 2 with the v1.7
pick, then one check per deep level the profile carries (4, 5, 6), each to 0.5 dB; a failing deep level
is nulled across all positions (the profile stands); the result is embedded as `tone_check` with
`deep_levels[]` and `deep_levels_nulled[]`, and the row in `cert/outcomes.json` is rewritten.

- **Resumable:** run the same command again; a profile whose tone check already carries `spec: v1.7`
  and its deep levels is skipped.
- **Licence:** a product the scan stopped is not loaded; a window during a check makes the row
  `needs_licence` (the export stands); `--retry-licence` re-checks those when the licence is back.
- **No sweeps, nothing sent.** A record without traces (none expected from the batch) is tone-checked on
  its existing points and says so.

## About how long

Per exported product: re-derivation 1–2 s, export under 1 s, then one probe process per level: g = 2
plus up to three deep levels, about 20–30 s each → **about 1–2 minutes per exported product**. A run
that exported 60 products is about an hour and a half; it can be stopped and resumed.

## What he zips back

Exactly as before: `cd ~/Library/ejmap && zip -rq ~/Desktop/ejmap-tonecheck-$(hostname -s)-$(date +%Y%m%d).zip cert`
— `cert/` only, never `config.json`. The profiles, their `.tonecheck.json`, the re-derived records and
`outcomes.json` are all inside.
