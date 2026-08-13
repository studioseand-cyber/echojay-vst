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

That count is the maps you swept — they're now safe off the machine.

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
