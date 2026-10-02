# EJ Map certification on a stranger's Mac — the mapper's runbook (2 Oct 2026)

This is the general loop and nothing else. It is judged against `docs/STRANGER_MAC_TEST.md`. You
need the packaged `ejmap.app` (made once per building Mac by `docs/PACKAGING_EJMAP_APP.md`); no
repo, no build tree, no path you edit. Every product-specific decision is made by the app and
written on the product's row; you never pass a product name, pick a candidate, or re-run one
plugin by hand. (The 1 Oct per-product runbook is archived as `archive_SEAN_MAC_RUNBOOK_2026-10-01.md`.)

**Conditions:** mains, lid open (the batch holds the Mac awake, but a closed lid still sleeps it).
ONE Waves version installed (12 or 15, never stacked). **The iLok absent is expected** — PACE
products are held, not tried. **NEVER press Send or Send All** in EJ Map: the map store is shared
and unprefixed, so anything sent lands in production. Certification sends nothing.

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap      # or wherever the packaged app was put
```

## 1. Pre-flight (10 seconds)

```
killall AudioComponentRegistrar 2>/dev/null           # only if plugins were just installed or changed: refreshes the AU list
"$BIN" --cert-preflight
```

It prints the probe it will use — the line must say **"beside the executable: the default"** — the
probe's Team ID, the cert root (`~/Library/ejmap/cert`), the ledger, the iLok and the power. Exit 0.
Anything else: stop, send the output back.

## 2. Sign in, scan, categorise (once per Mac, 5-10 minutes)

If this Mac has never run EJ Map: `open /Applications/ejmap.app`, sign in (the token lands in
`~/Library/ejmap/config.json`), quit. Then, headless:

```
"$BIN" --scan --categorise 2>&1 | tee ~/Library/ejmap/scan.log
```

That is the Scan button, then the Categorise button (one server round-trip to read categories and
map-state), then quit. It writes `~/Library/ejmap/scan-cache.xml`, `categories.json` and
`map-state.json` — what certification discovers from.

**The first scan can take a long time, and must be left alone.** It opens every VST3 bundle on the
Mac. A bundle that raises a licence or activation window (every iLok product with the iLok away;
expired and demo plugins) is killed at once by the window watch and listed as `needs_licence` —
seconds each, nothing is clicked, nothing retried. A bundle that hangs is quarantined by the
watchdog after its deadline (minutes each; the mapper's rule). After either, the app relaunches
itself and carries on from where it was. Do not click anything that appears; do not relaunch it
yourself; if it stops on its own, run the same command again and it resumes. The last lines say how
many rows were scanned, how many bundles were quarantined and how many need a licence.

When the licence is back: `"$BIN" --scan --retry-licence` re-probes only the `needs_licence` bundles.

## 3. The smoke check (one minute, before the batch)

```
"$BIN" --cert-sweep-census 2>&1 | grep -E "worklist:|RUNNABLE:|NOT RUNNABLE|EMO-D5"
grep -l '"name": "EMO-D5 (s)"' ~/Library/ejmap/maps/*.json 2>/dev/null
```

The check is **EMO-D5 (s) 15.0.70, map_fp `32b7e1d9a0c3…`**: the census line should read
`EMO-D5 (s)  DISCOVERED at 15.0.70, mapped (…)`, and if a local map exists its file is named by that
fp. This proves the Waves version, the AU list and the map keying agree. It is a smoke check, not a
procedure: EMO-D5 is then just one product in the batch.

If the census does not see products you expect, they have no map at their installed build (discovery
is keyed on maps: a local one, or one the server knows). Map them first — the normal mapping sweep,
local only:

```
"$BIN" --sweep --sweep-limit 50 2>&1 | tee -a ~/Library/ejmap/mapping.log     # opens unmapped plugins and writes maps into ~/Library/ejmap/maps/; never sends
"$BIN" --cert-sweep-census 2>&1 | grep -E "RUNNABLE:"
```

(Run it again with a larger limit until RUNNABLE stops growing. A plugin that hangs is skipped by
the supervisor and noted in the log.)

## 4. The batch (unattended; hours — see the cost table at the end)

```
caffeinate -i "$BIN" --cert-sweep-all --profile 2>&1 | tee -a ~/Library/ejmap/cert/batch.log
```

That is the whole loop. For every discovered compressor it samples the defaults, plans, sweeps the
profile grid (31 levels, quiet-reference ladder, engage search where the control does nothing, grid
refinement where the curve is bunched, the hold-doubled repeat), measures the detector, exports the
`ej_comp_profile/1` file and runs the section 8 tone check into it; for every discovered tuner it
writes the pitch record. It opens with the iLok's presence and the census (`cert/census.txt`), and
closes with the counts. It holds the Mac awake itself; `caffeinate -i` is belt and braces.

**It is resumable.** Power cut, sleep, ctrl-C, a crash: run the same command again. Finished
products keep their rows and files; unfinished ones complete; nothing is measured twice.

**It never needs you.** No `--product`, no `--candidate`, no `--include-pace`, no `--retry-refused`.
A product the rules cannot decide ends as `needs_review` with the reason, which is a correct result.

## 5. Read the result (one minute)

The last lines of the batch:

```
SWEEP-ALL: N attempted, ...
OUTCOMES (~/Library/ejmap/cert/outcomes.json): R rows - exported E, recorded T, refused F, held H, needs_review V
```

`R` must equal the number of discovered compressors and tuners (the census's RUNNABLE + held). Every
row has exactly one state:

| state | meaning | where to look |
|---|---|---|
| `exported` | an `ej_comp_profile/1` file with its tone check embedded | `cert/profiles/<Product>_<version>.json` (+ `.tonecheck.json`) |
| `recorded` | a tuner's pitch record | `cert/fixtures/<identity>.json` |
| `refused` | the measurement stopped at a named stage; the reason is on the row | `cert/fixtures/<identity>.json` (`thresholdRefusal`) |
| `held` | not measured on purpose: licence (PACE, no iLok) or hardware | the row |
| `needs_review` | measured, but a rule is missing or the result is not profile-grade (several threshold candidates; a flat sweep; an export the exporter refused) | the row's reason, then the record |
| `needs_licence` | the scan's window watch killed the bundle's load: an activation / licence window; never retried until `--scan --retry-licence` | `~/Library/ejmap/licence-stops.json` (bundle, windows, time) |
| `quarantined_at_scan` | the scan quarantined the bundle (a stall or a crash); the row names its products and category | `~/Library/ejmap/quarantine.json` |
| `unmapped` | installed and categorised a compressor or tuner, but no map at this build: the mapping sweep (step 3) maps it, then the batch measures it | the row |

A row with no reason, or an `exported` row without its files, is a bug: the batch exits 2 and says
`OUTCOME INVARIANT BROKEN`. Send that log back.

## 6. Send it back (one minute)

```
cd ~/Library/ejmap && zip -rq ~/Desktop/ejmap-cert-$(hostname -s)-$(date +%Y%m%d).zip cert
unzip -l ~/Desktop/ejmap-cert-*.zip | grep -c config.json      # must print 0
```

`cert/` only — never the whole `~/Library/ejmap` (its `config.json` holds the mapper token). Email the
zip. Nothing was sent to the map store at any point.

## Cost (measured on the operator's Mac, M1 Pro 16 GB, 2 Oct; refined by the dress rehearsal)

| what | time |
|---|---|
| pre-flight, census, smoke check | under 2 minutes |
| scan + categorise, ~900 plugins | 5-10 minutes |
| one compressor, profile run, no refinement (16 positions × 36 levels × 2 passes + reference) | 55-110 s |
| one compressor with engage search or grid refinement (up to 28 positions) | 120-200 s |
| detector + export + tone check per exported product | 30-40 s |
| a refusal (no threshold role) | 15-20 s |
| held (licence / hardware) | 0 (a row, no process) |
| one tuner | 60-120 s |

Rule of thumb: **2.5 minutes per discovered compressor, 1.5 per tuner, 0.3 per refusal.** A library
like the operator's (about 100 discovered compressors and tuners, 20 of them PACE-held) is 3-4
hours. A library twice that size is an evening, not a weekend. The batch can be stopped and resumed,
so it can run over several evenings.
