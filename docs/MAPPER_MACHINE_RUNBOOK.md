# Mapper machine runbook

Copy-paste checklist to map a machine's plugins and get the maps back to the
operator. Set this once per terminal window:

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
```

## The process — five steps

1. **Scan** — in the app.
2. **Categorise** — in the app (press Categorise; it works server-side as of
   13 Aug 2026). Fallback if the server's ever down: §A.
3. **Map one plugin by hand** — confirms the machine maps and saves (§2 below).
4. **Batch sweep** — the terminal loop (§3 below).
4b. **Certify compressors** — the second loop (§3b below), after the sweep.
    Needs mains, lid open, iLok in, and — on any Mac but the operator's — a
    signed ejmap.app (§3b's blocker).
5. **Zip and hand over** — zip `~/Library/ejmap`, send the zip to the operator
   (§4). **Do not rely on the app's Send All** — see §4 for why.

---

## §1 — Reset before sweeping

Clears the auto-resume marker (or every launch resumes the old crash-loop):

```
pkill -9 -f ejmap
rm -f ~/Library/ejmap/sweep-active.marker
```

## §2 — Map one plugin by hand

Open the app (it won't auto-sweep now), map a single plugin, confirm the count
goes up by 1:

```
ls ~/Library/ejmap/maps/ | wc -l
```

If that works the machine is healthy — move on. If not, stop and fix that first.

## §3 — Batch sweep

Small fresh batches — each a clean process that finishes and saves before it can
degrade. Leave it running:

```
for i in $(seq 1 60); do
  rm -f ~/Library/ejmap/sweep-active.marker
  "$BIN" --sweep --limit 25
done
```

Watch it climb in another tab (this is the real number; ignore the per-batch 0→25
counter):

```
ls ~/Library/ejmap/maps/ | wc -l
```

Resumable — Ctrl+C and re-run the loop anytime; it skips what's mapped. Stop when
the count plateaus.

### Skip a plugin that hangs

If a batch jams on one plugin (e.g. Guitar Rig 5, Acustica/Acqua), Ctrl+C it and
run this, swapping the name (lowercase, partial is fine). The next batch skips it:

```
python3 - <<'PY'
import json, os
p = os.path.expanduser('~/Library/ejmap/categories.json')
d = json.load(open(p)); n = 0
for k, v in d['products'].items():
    if 'guitar rig' in k.lower():
        v['disposition'] = 'operator_excluded'; v['why'] = 'hangs on load'; n += 1
tmp = p + '.tmp'; json.dump(d, open(tmp, 'w'), indent=1); os.replace(tmp, p)
print('excluded', n)
PY
```

Or leave it — the 90 s watchdog kills a hanger and quarantines it after 3 tries.

## §3b — Certify compressors (after the mapping sweep)

Runs the threshold sweep on every mapped compressor that has no certification
record yet. Same shape as §3: resumable, skips what is done, leave it running.
Nothing to type but the loop — the results land in `~/Library/ejmap/cert/` and
ride the §4 zip with everything else.

**Before starting, every time — these cost real hours twice:**

- **Mains.** Plug in and confirm; `pmset -g ps` must say `AC Power`. A sweep
  that runs across a sleep is refused and re-run once, so a lid-closed or
  battery-drained Mac wastes the run.
- **Lid open.** Keep it open for the whole batch.
- **iLok in.** `system_profiler SPUSBDataType | grep -A12 'iLok:' | grep 'Location ID'`
  must print a port. Note the port; if a PACE product refuses mid-batch, check
  whether it has changed — that is a drop, and the product needs `--retry-refused`.

```
for i in $(seq 1 20); do
  "$BIN" --cert-sweep-all --include-pace
done
```

Watch it climb in another tab:

```
ls ~/Library/ejmap/cert/fixtures/ | grep -vc defaults
```

What is left, and why the rest is not runnable:

```
"$BIN" --cert-sweep-census
```

Ctrl+C and re-run anytime. Stop when the census says nothing is runnable, or the
count plateaus. A product that stops (hangs, shows a window, has no threshold,
its reference is not the tone) writes a **refusal record** and is skipped from then
on — so a hanger costs at most a few minutes, once. The record says whether it is
**transient** (a fact about the run: hung, window, iLok out, slept, budget) or
**permanent** (a fact about the product: no threshold role, no ratio at 4:1).
After fixing what a transient refusal names (iLok back in, hardware attached),
re-run just those with:

```
"$BIN" --cert-sweep-all --include-pace --retry-refused
```

That skips the permanent ones on purpose; re-running them every batch is the
jam the record exists to escape. `--retry-refused-all` re-runs them too, for
the operator after a rule change.

A plugin you excluded in §3 (`operator_excluded`) is left alone here too.

### BLOCKER on any Mac but the operator's: ejmap.app is not signed

Certification hosts every plugin in a helper, `EchoJayProbe`, and **PACE refuses
a helper without a real Team Identifier** — the driver checks the signature and
stops before opening any plugin if it is ad-hoc or unsigned. Today nothing signs
or notarises `ejmap.app`: `RELEASE.md` signs the two **plugins** (EchoJay V2 and
Link), and the probe is a separate build artefact that ships nowhere. So on a
mapper's Mac, **no PACE-wrapped compressor can be certified until a signed and
notarised ejmap.app carrying the probe exists.** Native, licence-free products
still run, but only with a probe copied by hand.

What signing it needs, all recorded in `RELEASE.md` and all held on the
operator's machine:

- bundles/binaries: `Developer ID Application: Sean Donoghue (8BT5F9B887)`,
  hardened runtime, `--timestamp`, the probe's own
  `tools/au_instantiate_probe/EchoJayProbe.entitlements`
- notarisation: keychain profile `EchoJayNotarize` (team 8BT5F9B887), then
  `stapler staple`
- the probe goes **inside** `ejmap.app/Contents/MacOS/EchoJayProbe` (the sweep
  already looks for it there by default), so one `codesign --deep` covers both

**Open question, not answered:** mapper machines have been loading PACE plugins
through an unsigned ejmap.app for mapping. Either mapping tolerates something
certification does not (in-process hosting under a GUI app vs a headless helper),
or PACE products have been refusing on mappers' Macs and it went unnoticed in the
sweep's outcome classes. Check a mapper's `~/Library/ejmap` for maps of
PACE-wrapped products before assuming either.

## §4 — Zip and hand over (NOT Send All)

The app's **Send All is unreliable** and eats mapping sessions: it can lose its
upload URL, choke with a 413 on big maps, and crash mid-send. Don't fight it —
**zip the maps and hand them over**, and they get ingested on a keyed machine.
It's simpler, it can't lose work, and it dodges every send bug.

```
cd ~/Library && zip -rq ~/Desktop/carl-ejmap.zip ejmap && echo "size: $(du -h ~/Desktop/carl-ejmap.zip | cut -f1)"
```

(Swap `carl` for the machine/mapper name.) AirDrop / USB / upload that zip to the
operator. It contains the mapper token in `config.json`, so keep it **private**.
Verify the maps are in it before handing the machine back:

```
unzip -l ~/Desktop/carl-ejmap.zip | grep -c 'maps/.*\.json'
```

That count is the maps you swept — they're now safe off the machine. If §3b ran,
check its records are in too:

```
unzip -l ~/Desktop/carl-ejmap.zip | grep -c 'cert/fixtures/.*\.json'
```

---

## §A — Categorise locally (fallback only)

Only needed if the app's Categorise fails (the server fix is live, so normally it
just works). Needs the AI keys — trusted machine only, clear them after.
`categorise.py` is `tools/propose/categorise.py`.

```
python3 -m pip install --upgrade pip
python3 -m pip install --only-binary=:all: anthropic openai
export ANTHROPIC_API_KEY="…"
export OPENAI_API_KEY="…"
python3 categorise.py --limit 25
python3 categorise.py --report
python3 categorise.py
unset ANTHROPIC_API_KEY OPENAI_API_KEY
```

Writes `~/Library/ejmap/categories.json`. A harmless `KeyError: 'confidence'` in
the summary is fine — the categories are saved.

## §B — Exclude an unstable vendor

For a vendor whose install crashes the sweep (stacked Waves versions, Acustica).
Reversible.

```
cp ~/Library/ejmap/categories.json ~/Library/ejmap/categories.json.bak
python3 - <<'PY'
import json, os
p = os.path.expanduser('~/Library/ejmap/categories.json')
d = json.load(open(p)); n = 0
for k, v in d['products'].items():
    if 'waves' in str(v.get('vendor','')).lower() and v.get('disposition') == 'sweep':
        v['disposition'] = 'operator_excluded'; v['why'] = 'unstable vendor install'; n += 1
tmp = p + '.tmp'; json.dump(d, open(tmp, 'w'), indent=1); os.replace(tmp, p)
print('excluded', n)
PY
```

Undo: `cp ~/Library/ejmap/categories.json.bak ~/Library/ejmap/categories.json`

---

## Operator side — ingest a handed-over zip

Unzip it, then ingest the maps with the ingest script (POSTs each map to
`/api/params/ejmap` with the mapper token from its `config.json`, gzipping the
oversized ones so 413 can't bite). This replaces the app's send entirely. The
script is a scratch tool — if it's not to hand, ask Fable to (re-)emit it.

The proper in-app send fix (keep `upload_url`, gzip big maps, fix the mutex
crash) is still worth building, but the zip-and-ingest path is the reliable one
to lean on meanwhile.
