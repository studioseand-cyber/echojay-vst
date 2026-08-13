# Proposing over a mapper's maps

What to run on your machine after someone else has finished a sweep and pressed Send All.
Written 11 Aug 2026, from the run that processed Mac 2's 348 maps.

---

## Why this step exists

A mapper's maps arrive on the server as **controls-only**. Every control is dialable by exact
name the moment it lands, so their work is useful immediately. What's missing is the Tier 1
semantics, and those come from running both model arms over the map.

That runs on your machine, with your API keys. Nothing on the server does it yet.

---

## The sequence

All of this happens on your Mac. You never touch theirs.

```
cd "/Users/SeanD/Documents/ECHOJAY FILES/ECHOJAY VST/ejmap-wt"
```

### 1. Find and pull what has no proposal

```
python3 /tmp/find_unproposed.py ~/ejmap-pull
```

Writes `~/ejmap-pull/maps/`. Prints something like:

```
server holds            : 1290 fingerprint(s)
proposed on this machine: 1106
NO PROPOSAL HERE        : 184
```

If `/tmp/find_unproposed.py` has been cleaned away, ask Fable to re-emit it — it's a scratch
script, not part of the repo.

### 2. Price it before spending

```
python3 /tmp/preflight.py ~/ejmap-pull
```

Pass the root explicitly. It defaults to `~/Library/ejmap`, which will tell you there's nothing
to do. Calls no model, so it's free.

Roughly $0.0028 per control. 184 maps came to about $4.

### 3. Propose 25 first, and read them

```
python3 tools/propose/propose.py --root ~/ejmap-pull --limit 25
```

Then check a few before spending the rest:

```
python3 -c "
import json,glob,os
for f in sorted(glob.glob(os.path.expanduser('~/ejmap-pull/proposals/*.json')))[:3]:
    d=json.load(open(f))
    print(d.get('plugin'), '|', d.get('category'))
    for p in (d.get('params') or [])[:6]:
        print('   ', p.get('index'), p.get('kind'), p.get('confidence'), '-', str(p.get('reason'))[:60])
    print()"
```

You're looking for semantics that make sense for the plugin — a Studer's input and output as
dB, a Manley's cuts as frequency. If they look wrong, stop and look at the gates rather than
spending the rest.

### 4. Run the rest

```
python3 tools/propose/propose.py --root ~/ejmap-pull
```

Resumable — it skips anything that already has a `proposals/<fp>.json`. Roughly 50 minutes for
184 maps.

### 5. Upload the proposals

**This is the step that changes the corpus.** Everything before it is local.

The upload reads the token from a config in the scratch root, so copy yours in first:

```
cp ~/Library/ejmap/config.json ~/ejmap-pull/config.json
```

```
python3 tools/propose/upload_corpus.py --root ~/ejmap-pull --only-proposals --dry-run
python3 tools/propose/upload_corpus.py --root ~/ejmap-pull --only-proposals
```

`--only-proposals` skips the maps — they're already on the server, that's where they came from.

Wait for the verification line. It re-reads the server rather than trusting its own counters:

```
proposals by status: {200: 184}
VERIFYING by re-reading the server ...
  maps stored:      184 of 184
```

### 6. Copy back anything that was yours

If some of the pulled fingerprints are plugins you also have locally, your own corpus should
carry their proposals too:

```
python3 -c "
import json,glob,os,shutil
local={os.path.basename(p)[:-5] for p in glob.glob(os.path.expanduser('~/Library/ejmap/maps/*.json'))}
for p in glob.glob(os.path.expanduser('~/ejmap-pull/proposals/*.json')):
    fp=os.path.basename(p)[:-5]
    if fp in local:
        shutil.copy(p, os.path.expanduser('~/Library/ejmap/proposals/')); print('copied back', fp[:12])"
```

---

## What to expect

From the Mac 2 run: 184 maps, 2,103 controls, 596 accepted automatically (28%), 1,507
escalated, about 50 minutes and $4.

The escalations don't need working. An escalated control is still dialable by its own name —
the pile is upside rather than damage, and the field decides which parts are worth claiming.

---

## Things that will catch you out

**The count will be lower than what they swept.** A mapper sends every map they made, but any
fingerprint you already have a proposal for is skipped. Mac 2 sent 348 and only 184 needed
proposing, because 164 were plugins at versions your corpus already covered.

**`propose.py` has no `--dry-run`.** Use the preflight script instead.

**Pass `--root` to everything.** Both `preflight.py` and `upload_corpus.py` default to
`~/Library/ejmap` and will quietly report nothing to do.

**`upload_corpus.py` needs `config.json` in the scratch root.** It reads the token from there,
not from your library.

**Nothing reaches the corpus until step 5.** Proposing writes to disk. It's easy to finish
step 4, see the summary, and think you're done.

---

## What this doesn't cover

The arrival path. Nothing on the server proposes on ingest, so this runbook exists because you
run it. Fine for one or two mappers; at more than that it's worth building the queue worker
(ingest queues the fingerprint, a scheduled function drains it) so proposals happen without
you.
