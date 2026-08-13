# Mapper machine runbook — categorise + sweep

Copy-paste checklist to get a mapper's Mac producing maps. Do the parts in order.

> These are workarounds. The real fix for categorise is the server queue in
> `CATEGORISE_524_FIX.md` (not built yet). Until that ships, use Part 1.

Set this once per terminal window:

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
```

---

## Part 1 — Categorise locally

> **As of 13 Aug 2026 the server categorise fix is LIVE** — pressing Categorise in
> the app now works on any machine, no keys, no local run (see
> `CATEGORISE_524_FIX.md`). This Part is a **fallback only**: use it if the drainer
> kill switch (`config:categorise:disabled`) is on, or the server is down.

Do this when the app's **Categorise** button returns **524**. Needs the AI keys
(so only on a trusted machine; clear them after). `categorise.py` is
`tools/propose/categorise.py` — copy it onto the machine first.

```
# 1. libraries (--only-binary stops a pip compile-hang)
python3 -m pip install --upgrade pip
python3 -m pip install --only-binary=:all: anthropic openai

# 2. keys, this window only
export ANTHROPIC_API_KEY="…"
export OPENAI_API_KEY="…"
echo "A=${ANTHROPIC_API_KEY:+set} O=${OPENAI_API_KEY:+set}"     # want: A=set O=set

# 3. run it (test 25, read, then the rest)
python3 categorise.py --limit 25
python3 categorise.py --report
python3 categorise.py

# 4. clear the keys
unset ANTHROPIC_API_KEY OPENAI_API_KEY
```

Writes `~/Library/ejmap/categories.json`. A harmless `KeyError: 'confidence'`
in the end summary is fine — the categories are already saved.

---

## Part 2 — Reset the sweep

Always do this before sweeping — the marker is what makes it auto-resume the
crash-loop.

```
pkill -9 -f ejmap
rm -f ~/Library/ejmap/sweep-active.marker
```

---

## Part 3 — Map ONE plugin by hand

Open the app (it won't auto-sweep now), map a single plugin, and confirm the
count goes up by 1:

```
ls ~/Library/ejmap/maps/ | wc -l
```

If that works, the machine is healthy — move on. If it doesn't, stop and fix
that first; batching won't help.

---

## Part 4 — Batch sweep

Small fresh batches. Leave it running.

```
for i in $(seq 1 60); do
  rm -f ~/Library/ejmap/sweep-active.marker
  "$BIN" --sweep --limit 25
done
```

Watch it climb in another tab (this number is the truth; ignore the terminal's
per-batch 0→25 counter):

```
ls ~/Library/ejmap/maps/ | wc -l
```

Resumable — Ctrl+C and re-run the loop anytime; it skips what's mapped. When the
count stops rising, this worklist is done.

---

## Part 5 — Skip a plugin that hangs

If a batch jams on one plugin (e.g. Guitar Rig 5), Ctrl+C it and run this,
swapping the name (lowercase, partial is fine). Next batch skips it.

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

---

## Part 6 — Upload

```
"$BIN" --send-pending
```

---

## Optional — exclude a whole unstable vendor

For a vendor with a corrupt install (e.g. stacked Waves versions). Reversible.

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
