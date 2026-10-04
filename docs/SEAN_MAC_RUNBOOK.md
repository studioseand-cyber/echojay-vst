# EJ Map certification on a stranger's Mac — the mapper's runbook (2 Oct 2026)

This is the general loop and nothing else. It is judged against `docs/STRANGER_MAC_TEST.md`. You
need the packaged `ejmap.app` (made once per building Mac by `docs/PACKAGING_EJMAP_APP.md`); no
repo, no build tree, no path you edit. Every product-specific decision is made by the app and
written on the product's row; you never pass a product name, pick a candidate, or re-run one
plugin by hand. (The 1 Oct per-product runbook is archived as `archive_SEAN_MAC_RUNBOOK_2026-10-01.md`.)

**Conditions:** mains, lid open (the batch holds the Mac awake, but a closed lid still sleeps it).
ONE Waves version installed (12 or 15, never stacked). **The iLok absent is expected** — a licence
window at the scan is killed by the app and the product is `needs_licence`; at the batch the same.
**If a licence or activation window ever appears on screen and stays, Quit the app** (⌘Q) — never
click in it; run the same command again and the app carries on. **NEVER press Send or Send All** in
EJ Map: the map store is shared and unprefixed, so anything sent lands in production. Certification
sends nothing; the hand-over is a zip of `~/Library/ejmap/cert` only.

**The build.** `ejmap.app` is built once from branch `feat/ejmap-cert` at commit **`36397676`** (the
last code change; later commits on the branch are documentation) by `docs/PACKAGING_EJMAP_APP.md`.

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap      # or wherever the packaged app was put
```

## 0. Start from an empty ledger (once; 5 seconds)

If this Mac has run EJ Map before, `~/Library/ejmap` exists. MOVE it aside — never delete it:

```
[ -d ~/Library/ejmap ] && mv ~/Library/ejmap ~/Library/ejmap.before-cert-$(date +%Y%m%d-%H%M)
```

The run must start from an empty ledger: the scan, the categorise and the census are then this
Mac's own, and the result can be reconciled against them. (The sign-in in step 2 recreates
`~/Library/ejmap` with just the token.)

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
`EMO-D5 (s)  DISCOVERED at 15.0.70, map: …`, and if a local map exists its file is named by that fp.
This proves the Waves version, the AU list and the map keying agree. It is a smoke check, not a
procedure: EMO-D5 is then just one product in the batch.

**Expected result for EMO-D5 (s) in the batch** (measured here on V12 on 2 Oct): `RULE 1: 'Comp
Thresh' carries the compressor stage word alone`, the engage search finds `Comp On -> 1`, the pick
certifies, and the row is `exported` in **about 9 minutes** (521 s here), with the profile's notes
naming the rule, the pick, `Comp On -> On`, and the other stages (Gate / Leveller / DeEsser / Limiter)
at their instantiate values with their switches Off. A `needs_review` with five candidates means Rule 1
did not fire — send the record back.

**Mapping is optional for certification (ruled 2 Oct).** Certification does not need a map: the
batch samples each plugin's controls itself and computes `map_fp` exactly as EchoJay does, so the
server can join a profile to a map whenever the map arrives. The census's `NO MAP YET` line is
information. If you also want local maps (the mapper's normal work), the mapping sweep is the same
as ever and takes about 5 s per product — never Send:

```
"$BIN" --sweep --sweep-limit 50 2>&1 | tee -a ~/Library/ejmap/mapping.log     # local maps into ~/Library/ejmap/maps/; nothing sent
```

## 4. The batch (unattended; hours — see the cost table at the end)

```
mkdir -p ~/Library/ejmap/cert                     # tee needs the folder before the batch creates it (Sean's 3 Oct run had no batch.log for this reason)
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

**It never needs you.** No `--product`, no `--candidate`, no `--retry-refused`, and no `--include-pace`
(gone: PACE-wrapped is not unlicensed). A product whose bundle raised an activation window at the scan
is `needs_licence` in the batch too, carried forward and not loaded again; every other product is
tried, and the probe's own window watch catches a licence that vanished since the scan. When the
licence is back: `"$BIN" --cert-sweep-all --profile --retry-licence` re-checks only that set. A
product the rules cannot decide ends as `needs_review` with the reason, which is a correct result.

## 5. Read the result (one minute)

The last lines of the batch:

```
SWEEP-ALL: N attempted, ...
OUTCOMES (~/Library/ejmap/cert/outcomes.json): R rows - exported E, recorded T, refused F, held H, needs_review V
```

`R` covers every discovered compressor and tuner (the census's RUNNABLE + held) plus one row per
bundle the scan stopped or quarantined. Every row has exactly one state:

| state | meaning | where to look |
|---|---|---|
| `exported` | an `ej_comp_profile/1` file with its tone check embedded | `cert/profiles/<Product>_<version>.json` (+ `.tonecheck.json`) |
| `recorded` | a tuner's pitch record | `cert/fixtures/<identity>.json` |
| `refused` | the measurement stopped at a named stage; the reason is on the row | `cert/fixtures/<identity>.json` (`thresholdRefusal`) |
| `held` | not measured on purpose: licence (PACE, no iLok) or hardware | the row |
| `needs_review` | measured, but a rule is missing or the result is not profile-grade (several threshold candidates; a flat sweep; an export the exporter refused) | the row's reason, then the record |
| `needs_licence` | an activation / licence window — at the scan (carried forward, not loaded again) or at the probe's load in the batch; re-checked only by `--retry-licence` (scan: `--scan --retry-licence`; batch: `--cert-sweep-all --profile --retry-licence`) | `~/Library/ejmap/licence-stops.json`, the row |
| `quarantined_at_scan` | the scan quarantined the bundle (a stall or a crash); the row names its products and category | `~/Library/ejmap/quarantine.json` |
| `multiband` | "multiband: profiling not built yet" — band-numbered or Low/Mid/High threshold candidates (ruled 4 Oct); not a review item | the row |
| `surround` | "surround: not profiled" — more than two channels (Logic's `(N->N)`, N > 2); the profile is a stereo contract | the row |

Every row also carries `map`: `local map` / `server map state N` / `server map at a different build` /
`none` — information, never a gate. A carried-forward licence product has a product row AND the scan's
bundle row (two rows, one fact).

A row with no reason, or an `exported` row without its files, is a bug: the batch exits 2 and says
`OUTCOME INVARIANT BROKEN`. Send that log back.

## 6. Send it back (one minute)

```
cd ~/Library/ejmap && zip -rq ~/Desktop/ejmap-cert-$(hostname -s)-$(date +%Y%m%d).zip cert
unzip -l ~/Desktop/ejmap-cert-*.zip | grep -c config.json      # must print 0
```

`cert/` only — never the whole `~/Library/ejmap` (its `config.json` holds the mapper token). Email the
zip. Nothing was sent to the map store at any point.

## Cost (measured on the operator's Mac, M1 Pro 16 GB, mains, iLok absent — the dress rehearsal, 2 Oct)

| what | time |
|---|---|
| pre-flight + scan-watch self-test | 4 s |
| **scan + categorise, 1733 rows from empty** | **362 s** — of which 94 licence stops at a median 2.7 s each (319 s): on a Mac with many iLok products and no iLok the stops ARE the scan; 1073 catalogue answers took seconds |
| census | 7 s |
| one tuner | 13 s |
| one compressor, profile run (16 positions, 36 levels, 2 passes) | 60–125 s |
| one compressor with an engage search or grid refinement | 120–200 s |
| **one channel strip (several threshold candidates, every switch tried)** | **~2600 s** (EMO-D5: 43 minutes to end as needs_review — the one-candidate rule, when it lands, cuts this) |
| a refusal (no threshold role) | 15 s |
| carried-forward licence / hardware held | 0 s (a row, no process) |
| mapping one product locally (optional) | 5 s |
| zip | 1 s |

Rule of thumb: **2 minutes per compressor, 15 s per tuner, 45 minutes per channel strip, 3 s per
licence stop, 6 minutes for the scan.** The operator's library (≈120 runnable compressors and tuners, of
which 17 PACE and a handful of channel strips) is about 4–6 hours end to end; a library twice that size is
an evening and a morning, not a weekend. The batch resumes after any interruption, so it can run over
several evenings; the channel strips are the long pole until the one-candidate rule lands.
