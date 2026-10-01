# Running EJ Map certification on Sean's Mac (Waves 15.0.70, his Developer ID, no iLok)

Written 1 Oct 2026 from the real commands on Kathy's Mac, branch `feat/ejmap-cert`. Every
command below was run here exactly as written unless marked **[his Mac only]**. Where a step
could not be tested here, it says so and says why.

Set these once per terminal:

```
REPO="$HOME/echojay-vst"          # wherever you clone it; the JUCE checkout must be its SIBLING: $HOME/JUCE
BIN="$REPO/build-ejmap/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app/Contents/MacOS/ejmap"
PROBE="$REPO/build-ejmap/EchoJayProbe_artefacts/RelWithDebInfo/EchoJayProbe"
```

## a. Get the code  — tested here

```
git clone https://github.com/studioseand-cyber/echojay-vst.git "$REPO"
cd "$REPO" && git checkout feat/ejmap-cert && git log --oneline -1
```

The commit this runbook was written against: **`9a01b170`** ("Fold v1.3 and v1.4 into the armed
profile run…") or later on the same branch. `git pull` is fine; nothing here depends on a
specific later commit.

JUCE **8.0.12** must sit beside the checkout as `$HOME/JUCE` (the root `CMakeLists.txt` reads
`JUCE_PATH = ${CMAKE_SOURCE_DIR}/../JUCE`; override with `-DJUCE_PATH=/path/to/JUCE`). The AAX SDK
is optional: without it the configure prints "AAX SDK not found — building without AAX" and
carries on; neither the ejmap app nor the probe needs it.

## b. Build the ejmap app and EchoJayProbe  — tested here (from-scratch configure + build, 1 Oct 21:35, and a configure without the AAX SDK)

One build tree, configured from the repo root (the root CMake owns `tools/ejmap` and
`tools/au_instantiate_probe`); both targets come out of it:

```
cd "$REPO"
cmake -S . -B build-ejmap -DCMAKE_BUILD_TYPE=RelWithDebInfo -DJUCE_PATH="$HOME/JUCE"
cmake --build build-ejmap --target ejmap EchoJayProbe -j 4
ls -l "$BIN" "$PROBE"
```

Configure prints `AAX SDK not found — building without AAX` on a Mac without the SDK and carries
on (tested here with `-DAAX_SDK_PATH=/nonexistent`). The from-scratch build took about 15 minutes
at `-j 4` on an M1 Pro.

`-j 4`, not `-j`: an unlimited make on a 16 GB Mac has crashed this build before. The driver
looks for the probe beside `ejmap` by default; on a built tree it is not there, so every driver
command below passes `--probe "$PROBE"` explicitly.

## c. Sign the probe  — tested here (the same identity signs here)

PACE refuses a probe without a real Developer ID Team Identifier, and so does the driver (it
runs the verify below before it opens any plugin). Sign once, after every rebuild of the probe:

```
cd "$REPO"
codesign -f -s "Developer ID Application: Sean Donoghue (8BT5F9B887)" --timestamp --options runtime \
  --entitlements tools/au_instantiate_probe/EchoJayProbe.entitlements "$PROBE"
codesign --verify --strict "$PROBE" && echo "probe verifies"
codesign -dvvv "$PROBE" 2>&1 | grep -E "^TeamIdentifier|^CDHash"
```

`TeamIdentifier=8BT5F9B887` must print; `TeamIdentifier=not set` means ad-hoc, and the driver
will refuse with "PACE needs a Developer ID". If `codesign` sits for more than a few seconds it
is waiting on a keychain prompt — unlock the login keychain and run it again. The CDHash is the
probe's identity in every record (`probe` field).

## d. Pre-flight  — each command tested here except where marked

**Exactly one Waves version installed.** Waves ships one AU component per shell; list them:

```
ls /Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/Components 2>/dev/null | grep -i WaveShell
```

One version means every line reads the same major (e.g. `WaveShell1-AU 15.x`). Kathy's Mac
prints 12.0, 12.1, 12.4, 12.5, 12.6 — that is what "stacked" looks like, and it is why nothing
Waves is certified on V12 here. If yours shows anything but 15.x, STOP and remove or exclude it
before sweeping: a product that resolves through two shells is refused as ambiguous, and one that
resolves through the wrong one measures the wrong binary.

**Mains, lid open.**

```
pmset -g ps | head -1            # must say: Now drawing from 'AC Power'
ioreg -r -k AppleClamshellState -d 4 | grep AppleClamshellState   # must say: = No
```

A sweep that runs across a sleep is refused and re-run once; on battery the Mac WILL sleep
mid-batch (it did here twice). Keep it plugged in and open for the whole run.

**No iLok, so PACE products refuse — expected.** Any product that is PACE-wrapped (SSL Native,
Softube, kHs, Antares, Tube-Tech CL 1B…) will show PACE's activation window at the first probe
and be recorded as a `transient` refusal ("UNLICENSED ON HOST"). That is correct behaviour, not a
setup fault; Kathy certifies those here with the iLok. Waves is not PACE.

**NEVER press Send or Send All in EJ Map on your Mac.** The map store on the server is shared and
unprefixed: anything sent from the GUI lands in production under the shared ingest token.
Certification sends nothing — every command below is local — and the results go back as a zip
(step i). This cannot be tested here without sending; it is a rule, not a check.

## e. First run on a fresh machine  — tested here (GUI steps are the runbook's §1-§2)

```
killall AudioComponentRegistrar 2>/dev/null     # if Waves was just installed or updated: a stale AU cache hands EJ Map the old list
open "$REPO/build-ejmap/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app"
```

In the app: **Scan**, then **Categorise** (server-side; one connected run). That writes
`~/Library/ejmap/scan-cache.xml`, `categories.json` and `map-state.json`, which is what
certification discovers from. Quit the app afterwards; the driver runs headless. Do not press
Sweep All or Send All.

## f. Census, and confirm EMO-D5 (s) is 15.0.70  — the census tested here on a scratch ledger

```
"$BIN" --cert-sweep-census 2>&1 | grep -E "worklist:|RUNNABLE:|EMO-D5"
```

The store defaults to `~/Library/ejmap/cert/fixtures` (empty on a fresh machine, which is right).
A good line:

```
  EMO-D5 (s)      DISCOVERED at 15.0.70, mapped (server map state 3) (defaults first; plan after sampling)
```

(Here it reads `12.0.0`.) **If EMO-D5 (s) is not in the census** it is because discovery is
keyed on MAPS: the product must have a map at ITS installed build — a local one in
`~/Library/ejmap/maps/`, or one the server knows (`map-state.json` state 1–3). A V12 map does not
count for V15. Then: run the runbook's §3 mapping sweep for it first, in the app (`Sweep All`
is fine LOCALLY; just never Send), or from the terminal:

```
"$BIN" --sweep --sweep-limit 25      # maps up to 25 unmapped plugins into ~/Library/ejmap/maps/ (add --dry-run to see what it would open)
"$BIN" --cert-sweep-census 2>&1 | grep "EMO-D5"
```

(The flag is `--sweep-limit`; the mapper runbook had `--limit`, which the app does not read —
corrected 1 Oct after a dry run here.)

**Confirming the fingerprint.** The map file's NAME is the fp:

```
grep -l '"name": "EMO-D5 (s)"' ~/Library/ejmap/maps/*.json
```

should print a file named `32b7e1d9a0c3….json` (the 64-hex `map_fp` that EchoJay logs as
`fp=32b7e1d9a0c3`). The certification record carries the same value as `map_fp` once the
defaults pass has run (next step); `param_count` beside it is the number the hash is built from.

**Native or bridged?** Each record says: `thresholdSweep.host` reads `.../arm64/...` and
`bridged: false` for a native V15, `bridged: true` (x86_64 under Rosetta) for V12. Write landing
differs between the two (`positionLandedBy` per position), so note which you got.

## g. EMO-D5 (s) alone: engage, the v1.4 profile run, tone check, export  — commands tested here on CL 1B / batch 8; EMO-D5 itself [his Mac only]

Everything lands under `~/Library/ejmap/cert/`: `fixtures/<identity>.json` (the record),
`raw/` (one text file per probe process), `<identity>.sweep.processes.json`, `run.jsonl`,
`<identity>.sweep.report.txt`.

```
mkdir -p ~/Library/ejmap/cert
"$BIN" --cert-sweep --profile --product "EMO-D5 (s)" --probe "$PROBE" 2>&1 | tee ~/Library/ejmap/cert/emo-d5-s.log
```

That one command does, in order: the defaults pass (`param_count`, `map_fp`), the plan (threshold
candidates by role), the neutral set (mix 100 % wet, make-up 0, auto make-up off — each read back
and recorded as `preconditions[]`), then per candidate: the 31-level sweep (−60..0 dBFS peak, 2 dB
steps, 2.5 s hold / last 300 ms, ascending in each fresh process, the quiet reference at every
position), **engage detection** when a candidate reads pass-through (each `…On` switch tried in a
quick probe; EMO-D5 found `Comp On`, `Gate On`, `Limiter On` here on the first candidate each),
the full sweep with the verified write, and **the hold-doubled repeat** (5 s) for
`quality.point_error_db`. Expect roughly 10–20 min; the log says which candidate is running.

Then the detector fraction (two-tone at the same RMS, at the compressing position nearest −18 dBFS
RMS), written into the record:

```
REC=$(grep -l '"product": "EMO-D5 (s)"' ~/Library/ejmap/cert/fixtures/*.json)
"$BIN" --cert-detector "$REC" --probe "$PROBE"
```

Export (v1.4; refuses with a reason if anything required is missing):

```
mkdir -p ~/Library/ejmap/cert/export
"$BIN" --export-profile "$REC" ~/Library/ejmap/cert/export/EMO-D5_s.json
```

Tone check (his section 8, run before he does): picks the position by section 6 from the
EXPORTED profile, writes engage + neutral + the reference ratio, renders 997 Hz at L = −18 dBFS
RMS, measures GR; pass within 0.5 dB of 2:

```
"$BIN" --cert-tone-check ~/Library/ejmap/cert/export/EMO-D5_s.json "$REC" --probe "$PROBE"
```

writes `~/Library/ejmap/cert/export/EMO-D5_s.tonecheck.json` beside the profile.

**EMO-D5 has FIVE threshold candidates** (Gate / Comp / Leveller / DeEsser / Limiter), so its
record is a `thresholdCandidates` record and, unpicked, exports as "topology other". The human
pick his section 3 asks for is `--candidate "Comp Thresh"`, accepted by all three commands; the
export then carries `pickedCandidate` and topology `threshold`. So for EMO-D5 the three commands
above are:

```
"$BIN" --cert-detector   "$REC" --probe "$PROBE" --candidate "Comp Thresh"
"$BIN" --export-profile  "$REC" ~/Library/ejmap/cert/export/EMO-D5_s.json --candidate "Comp Thresh"
"$BIN" --cert-tone-check ~/Library/ejmap/cert/export/EMO-D5_s.json "$REC" --probe "$PROBE" --candidate "Comp Thresh"
```

(The pick is pinned in the suite, C1–C5; tested here on a synthetic five-candidate record, not
yet on EMO-D5 15.0.70 — that is the run itself.)

## h. What a good result looks like, and what each refusal means

A good export, read from the file (the template Kathy fills for CL 1B):

```
<product> <version>  map_fp <64 hex>
reference_ratio: <read-back ratio>   detector_f: <0..1>
points with numeric in_at_gr["2"]: <n> of <positions>
quality.point_error_db (2.5 s vs 5 s): <dB>        (server gate: 0.5)
fit.max_error_db (informational): <dB>
monotonic self-check: pass/fail
tone check (997 Hz at L = -18 RMS, g = 2): measured GR <dB> -> pass/fail (0.5 dB)
```

Refusals you can meet, and what they are:

| message | meaning | setup or real? |
|---|---|---|
| `ABORTED BEFORE ANY PLUGIN - the probe is not validly signed` / `no Team Identifier` | step c not done, or ad-hoc signed | setup |
| `'X' is not on the worklist` | not discovered: no map at the installed build, or a record already exists | setup (map it, or `--retry-refused`) |
| `UNLICENSED ON HOST: … SHOWED A WINDOW (PACEEdenExperience)` | PACE product, no iLok here | expected on your Mac |
| `refusal recorded at stage 'defaults' (transient)` | the plugin would not list/instantiate in the probe (timeout, crash) | real; re-run once with `--retry-refused` |
| `not swept: 0 controls hold the threshold role` (stage `plan`, permanent) | no control named like a threshold | real |
| `the soft end is not linear …` then `re-sweeping with the quiet-level reference` | normal: the fallback | real, handled |
| `stopped after 2 unclean processes (budget)` | the plugin hangs or crashes per process | real; transient |
| `pass-through at defaults: trying N engage candidate(s)` then `engage verified: …` | normal | real, handled |
| export: `detector_f not measured` | run `--cert-detector` first | setup |
| export: `quality.point_error_db not measured` | the record is from a non-profile sweep; re-run with `--profile` | setup |
| export: `only N curve point(s) reach 1 dB` | the threshold's range leaves fewer than 9 positions inside −60..0 | real |
| export: `topology other: several threshold candidates` | multi-threshold product unpicked (EMO-D5 is one) | pass `--candidate "Comp Thresh"` |
| `'Nope' is not a candidate of this record (candidates: …)` | the pick named a control that is not a threshold candidate | setup; use a listed name |
| tone check: `FAIL` with `quiet check FAILED` | the chosen position compresses at −48 dBFS already | real |

## i. Sending results back  — tested here (the zip excludes the token)

Zip ONLY the cert directory. `~/Library/ejmap/config.json` holds `ingest_token` and
`mapper_token`; the whole-ledger zip in the mapper runbook is for maps and must not travel for this.

```
cd ~/Library/ejmap && zip -rq ~/Desktop/sean-cert.zip cert && unzip -l ~/Desktop/sean-cert.zip | grep -c json
```

Send `~/Desktop/sean-cert.zip`. It contains the records, the raw traces (so any rule can be
re-derived here without re-measuring), the exports and the tone-check results, and nothing else.

## Tested here / his Mac only — the honest list

Tested on Kathy's Mac, 1 Oct: a, b (from-scratch configure and build of both targets into a
scratch tree, and a configure with no AAX SDK), c (sign + verify + identity print), d (the four
commands), e's terminal lines, f's census on a scratch copy of this ledger with an empty store and
the mapping sweep's `--dry-run`, g's commands on CL 1B and on batch 8's engage products (the
`--candidate` pick on a synthetic record), h's refusal texts (each one seen in a real run), i's zip (on a scratch ledger laid out like ~/Library/ejmap: the zip holds cert/ and not config.json).
Only on his Mac: a single Waves 15 install, EMO-D5 (s) at 15.0.70 and its fingerprint
32b7e1d9a0c3…, native-vs-bridged for V15, the GUI Scan/Categorise on a machine that has never
mapped, and the EMO-D5 profile run itself.
