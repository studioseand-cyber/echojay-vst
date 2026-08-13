# EchoJay parameter mapper — handover

Written 11 Aug 2026. Everything here is measured unless it says otherwise.

You are picking up a project that has been running intensively for about a week. The work is
in a good state: the core thing works end to end and has been proven across two machines. What
remains is mostly polish, one architectural gap, and a release.

Read this whole document before running anything. Several of the traps below cost a full day
each, and every one of them looked like something else at the time.

---

## 1. What the thing is

**EchoJay** is an AI mix-feedback product: a JUCE plugin (AU/VST3) that hosts other people's
plugins in a rack, plus a Next.js/Vercel web app and API.

The feature this work serves: a user types "give me a fast attack and 6:1 on the SSL Blitzer"
and EchoJay dials the actual plugin. To do that it needs to know what every parameter of every
plugin does.

**ejmap** is the tool that builds that knowledge. It scans a machine's plugins, loads each one,
sweeps its parameters, and writes a **map** — a JSON file describing every control, its range,
its unit, and an anchor table of measured display values.

The corpus is the defensible asset. It currently holds **~1,290 maps** on the server.

### The two ways a control can be dialed

- **Tier 2, by name.** The map records that a plugin has a control called `Sustain`. The model
  is told the names of controls on racked plugins, emits `{"controls":{"Sustain":4}}`, and the
  client resolves it by exact name. Works for all 51,000+ controls in the corpus.
- **Tier 1, by semantic.** The map records that index 3 *is* `threshold_db`. This lets "set the
  threshold to -20" work on any compressor without the model knowing the product.

**Tier 1 was retired on 9 August.** All 34 hand-mapped plugins had their 124 semantic rows
demoted to named controls with `human-verified` trust. There is one path now: name-addressed.
Model-proposed semantics still exist in proposals and are served, but nothing is hand-mapped
any more and nothing should be.

---

## 2. Where the code lives

There are several worktrees. **Which tree ships a file matters more than which file you edit** —
this cost a day.

| Path | Branch | What it owns |
|---|---|---|
| `~/Documents/ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200` | `feat/plugin-dashboard` | **The plugin. Authoritative.** |
| `~/Documents/ECHOJAY FILES/ECHOJAY VST/ejmap-wt` | `feat/ejmap` | The ejmap tool and the propose pipeline |
| `~/Documents/ECHOJAY FILES/ECHOJAY WEB APP/echojay-saas-dialable` | `pricing-v2` | **The server. Deploys from here.** |

Also present: `WORKTREES/vst-stream`, `WORKTREES/saas-classifier`, `WORKTREES/vst-classifier`
and others. Some are superseded and already merged; check before working in one.

`ejmap-wt` contains a copy of the plugin source that is **two minor versions behind and must
never be built or installed**. A day was lost installing a plugin built from it.

### Docs that already exist and are worth reading

- `echojay-vst-v200/HANDOVER.md` — the plugin-side handover, with commit hashes
- `ejmap-wt/docs/TREES.md` — which tree is authoritative for what
- `ejmap-wt/docs/DIAL_TEST_RIG.md` — how to test the dial path
- `ejmap-wt/docs/EJMAP_MERGE_QUEUE.md` — deferred work
- `ejmap-wt/docs/RECORDED_IS_COUNTED.md` — the quarantine counting fix
- `ejmap-wt/docs/correct-work-repeated.md` — a defect class that recurred three times

### First thing when opening any tree

```
git worktree list
git status
git rev-list --left-right --count feat/plugin-dashboard...HEAD
```

If the first number is non-zero you are behind and must rebase before touching `Source/`. If
you find uncommitted work that isn't yours, **stop and say so** — never commit, stash or clean
it. Three sessions have run in parallel and their work has been mixed before.

---

## 3. The pipeline, end to end

```
Scan          enumerate the machine's plugins
Categorise    ask the server what category each product is (server holds the AI keys)
Sweep         load each plugin, sweep its parameters, write a map locally
Send All      upload maps to the server
Propose       two AI models read each map and propose Tier 1 semantics   ← runs on Sean's Mac
Upload        push proposals back to the server
```

Everything up to Send All happens in the app on the mapper's machine, no terminal.

**Propose is the gap.** It runs locally with Sean's API keys. A mapper's maps land on the
server and nothing proposes over them until someone pulls them down and runs the pipeline. See
§8.

### The propose stage in detail

Two arms — `claude-opus-5` and `gpt-5.5` — independently propose a semantic for each control
from its name, index, kind, range, unit and anchor summary. Where both agree and both are
confident, it's written automatically. Anything else escalates to a review pile.

Measured accuracy against 125 hand-confirmed answers: **98.7%** on both-confident agreements.
Hedged agreements measured 97.7%, statistically indistinguishable, so they were folded into
the accept set.

**A third arm was tested and rejected** (4 Aug). It settled nearly every disagreement while
being wrong most of the time — 6 escalations became 2 correct writes and 3 silently wrong ones,
where all 6 reaching a human produced 6 right answers. Models sharing training share failure
modes; a third arm launders uncertainty into a majority. `--three-arm` exists in the tooling and
has never been run in production. Don't.

**The review pile does not need working.** An escalated control is still dialable by name, so
the pile is upside rather than damage. 266 rows from the 40-map pilot were reviewed by hand;
the corpus's ~6,700 never were and that's fine.

---

## 4. Key concepts and how things are keyed

Getting these wrong causes silent, plausible-looking failures. Three separate incidents.

| Key | Shape | Used for |
|---|---|---|
| **fingerprint** | `SHA256(format\|uid\|version\|paramCount)` | Maps. Needs a loaded instance — a scan cannot compute one. |
| **identity** | `format\|uid\|version` | What the worklist checks; what the server returns from `identities=` |
| **product** | `format\|uid` (version dropped) | Categories, unmappable marks |
| **plugin_id** | `format,uid,manufacturer` | The ledger, quarantine, retry counting |

Consequences:

- Maps are per fingerprint, so **an AU and a VST3 of the same plugin are two maps**, and a
  version bump makes a third. This is correct — UAD 2.0 vs 3.0 changed parameter counts, e.g.
  Ampeg B15N went from 20 to 41.
- Categories are per product, so one entry covers every variant. This is why Mac 2 needed 140
  new categories but 1,460 new maps.
- Fingerprints are **deterministic across machines** — proven 10 Aug: Mac 2 independently
  computed `e83a8cb3aa24...` for SSL Blitzer AU, matching Mac 1.

---

## 5. Where things stand

### Proven working

- **Cross-machine mapping.** Mac 2 scanned 2,012 plugins, skipped 552 already on the server,
  categorised 140 from cache at zero cost, swept 348 maps, sent them, and 184 were proposed and
  uploaded from Mac 1. None of it required touching Mac 2 after it finished.
- **The dial path.** SSL Blitzer and bx_townhouse both dial correctly from natural language.
  Verified from probe records, not from the screen.
- **The honesty floor.** No surface claims a dial before one happens. Silence means success.
  A wrong or partial write says so. This does not depend on the model cooperating.
- **Mapper auth.** Per-mapper tokens, hashed in Redis, revocable at runtime. Attribution lands
  correctly — Mac 2's submissions carry its own ref.

### Known gaps

1. **No arrival path.** Proposals only run on Sean's machine. See §8.
2. **The GUI Sweep All button behaves worse than `--sweep` from a terminal.** Both paths are
   the same code and the difference is unexplained. Terminal is the recommended route for now.
3. **UAD plugins show a modal on instantiation** ("no UAD hardware detected" or a demo prompt)
   that blocks headless sweeps. They time out after 90s and quarantine after three. On Mac 2
   this cost most of a night. Excluding the vendor is one command (§9).
4. **The captures toggle persists between runs** and silently changes behaviour. Captures mode
   opens editors, which is what raises vendor dialogs.
5. **Screenshot/vision stage** is designed but deliberately deferred. Captures are free during a
   sweep (the plugin is already open); bulk vision is not worth it. Waves panels capture empty
   for reasons that are measured but unexplained.
6. **26 maps exceed Upstash's 10MB request limit** in `pmap2:map:*` — a key that is written and
   never read. Harmless today, worth tidying.

### The release

The plugin is at **2.26.0**, built but **not signed or notarized**. Three server-side feature
gates (`SET_OP_MIN_PLUGIN_VERSION`, `BANDS_MIN`, `CONTROLS_MIN`) are pinned at `2.99.0`, a
sentinel no real build reaches. They must be moved to `2.26.0` **in the same window** as the
client release, or dialing works for nobody.

Do not revert the version. A 2.26.0 build not dialing is the gate working, not a regression.

---

## 6. Rules that were paid for

Each of these came from a defect that cost hours or days. They are the most valuable part of
this document.

**Check which tree ships a file before editing it.** Not just that the file is the one you
meant. A day was lost editing a plugin source 58 commits behind the one that ships.

**Verify artefact identity, not filename or version string.** Compare SHA-256 of source and
destination. An `mtime` says something was written; a matching digest says the thing running is
the thing you compiled. A "backup" that was a symlink to the live corpus passed every check
because the check followed the link.

**A substring match is not an identity check.** `grep -c "attended"` matched `unattended` and
produced a confident wrong conclusion about which binary was installed.

**An absence is only evidence once you know what would make the thing present.** Three
instances in one session. `fetchFailure: none` meant "nobody ever asked", not "the fetch was
fine", because the cache was never read.

**Assert on content, not completion.** A sweep reported 153 plugins mapped having swept
nothing — it restored completed sessions and re-submitted them. Every counter was green.

**Score a filter's discards, not just its accepts.** A confidence gate was manufacturing 34% of
the review pile. Nobody measured what it rejected until someone asked "would the things I'm
rejecting have been right?" — $2 against data already on disk, never asked in weeks of building
machinery for that pile.

**Correct work, repeated, is a cost.** Three instances: a directory walk per identity, 783
declines re-derived on every restart, 74.9MB of JSON re-parsed per plugin. Nothing is wrong with
any single pass; the aggregate is most of the wall time.

**Recorded is not counted.** Every failure path wrote a ledger row but only one branch counted
toward quarantine, so 13 rows of 144,911 carried the retry arithmetic and repeat crashers looped
forever. Evidence accumulated in a file nobody consulted.

**Instruction is a ceiling, never a floor.** Four rounds of prompt changes each produced visible
surface improvement and none guaranteed anything. What worked was handing the model a *decided
fact* rather than a rule: 6/6 clean under a computed fact against 3 substitutions in 5 under
rules.

**A metric instrumented at the failure site cannot count compliance.** The counter must live at
the decision, not the violation handler, because compliance often means the guarded path never
runs.

**Demonstration placement carries weight that description beside does not.** A MUST-sentence
next to an example loses to the schema-authoritative position every time.

**The defect can live between two components that are each individually correct.** Two prompt
notes written the same day, each complete, never connected — and nothing in either was wrong.

**`dev.json` overrides the endpoint silently.** `~/.echojay/dev.json` pins the client to a
preview deployment. Preview URLs are immutable builds, so a stale one serves old code forever
and nothing on screen says which server is being used. This cost two days once and half a day
again.

---

## 7. Operational procedures

### Deploying the server

Only Sean deploys production. Preview deploys at explicit request are fine.

```
cd ~/Documents/"ECHOJAY FILES"/"ECHOJAY WEB APP"/echojay-saas-dialable
git push origin pricing-v2
vercel --prod          # production
vercel                 # preview — prints a new immutable URL
```

**One line at a time.** Pasting a block queues the rest as input to confirmation prompts. A
home-directory deploy nearly went through this way.

After a preview deploy, `~/.echojay/dev.json` must be repointed to the new URL or the client
keeps talking to the old build.

### Installing the plugin

Quit Logic first. Then:

```
cd ~/Documents/"ECHOJAY FILES"/"ECHOJAY VST"/echojay-vst-v200
SRC="build-dev/EchoJay_artefacts/Release/AU/EchoJay V2.component"
DST=~/Library/Audio/Plug-Ins/Components/"EchoJay V2.component"
rm -rf "$DST" && cp -R "$SRC" "$DST"
shasum -a 256 "$SRC/Contents/MacOS/EchoJay V2" "$DST/Contents/MacOS/EchoJay V2"
auval -v aufx EcJ2 Ecjy | tail -3
```

The two digests must match. `auval` must PASS.

### Running a sweep

```
BIN=/Applications/ejmap.app/Contents/MacOS/ejmap
pkill -9 -f ejmap
"$BIN" --worklist 2>&1 | head -3      # what would open
"$BIN" --sweep                        # run it
"$BIN" --send-pending                 # upload
"$BIN" --sweep-report --all           # what actually happened
```

Only one instance can run — the app holds a lock. `pkill -9 -f ejmap` before terminal commands.

**Read `--sweep-report`, not the progress bar.** The percentage resets on every crash because it
counts position in a *shrinking* list. A quarantine withdrawal presents identically to a loop.
The only difference between working and stuck is whether the worklist total goes down.

---

## 8. The arrival path (the main architectural gap)

A mapper's maps reach the server. Nothing proposes over them.

Today's workaround, which works and is documented in
`propose-after-a-mapper-sends.md`: pull the unproposed fingerprints to a scratch root, run
`propose.py --root`, upload with `--only-proposals`. About 50 minutes and $4 per 184 maps.

Fine for one or two mappers. It does not scale past Sean running a script for each of them.

**The fix**: ingest writes the fingerprint to a Redis queue; a Vercel cron or background
function drains it a few maps at a time, each invocation well inside the 300s ceiling, with
retries at the queue level. Fable scoped this and confirmed it is viable without Modal. Not
built.

---

## 9. Snippets you will want

**Exclude a vendor from sweeping** (UAD is the usual case):

```
python3 - <<'PY'
import json, os
p = os.path.expanduser('~/Library/ejmap/categories.json')
d = json.load(open(p)); n = 0
for k, v in d['products'].items():
    if k.split('|')[-1].strip() == 'universal audio' and v.get('disposition') == 'sweep':
        v['disposition'] = 'operator_excluded'
        v['why'] = 'UAD alert on instantiate blocks unattended sweeps'
        n += 1
json.dump(d, open(p, 'w'), indent=1); print('excluded', n)
PY
```

**Release everything from quarantine:**

```
EJ=/Applications/ejmap.app/Contents/MacOS/ejmap
python3 -c "
import json,os
q=json.load(open(os.path.expanduser('~/Library/ejmap/quarantine.json')))
print('\n'.join(e['plugin_id'] for e in q))" | while IFS= read -r id; do
  "$EJ" --release-quarantine "$id"
done
```

**Check a sweep is actually progressing:**

```
ls ~/Library/ejmap/maps/ | wc -l     # now, and again in ten minutes
```

**Where things live on disk:**

```
~/Library/ejmap/maps/            the maps, named by fingerprint
~/Library/ejmap/proposals/       proposals, named by fingerprint
~/Library/ejmap/ledger.json      every event, ~56MB
~/Library/ejmap/quarantine.json  withdrawn plugins
~/Library/ejmap/categories.json  product → category
~/Library/ejmap/config.json      mapper token, upload_url (chmod 600)
~/Library/ejmap/tester.json      required for submit, no UI for it
~/.echojay/dev.json              endpoint override — check this first when anything is odd
```

---

## 10. Credentials — read this

The machine you are working on holds live credentials.

- `~/Library/ejmap/config.json` — a mapper token that can write to the corpus
- `~/.zshrc` / `~/.bash_profile` — `ANTHROPIC_API_KEY` and `OPENAI_API_KEY`, both billable
- `~/Desktop/ejmap-mac2/` — a distributable package **containing a live mapper token**
- Vercel env vars, pulled with `vercel env pull .env.local`, include the read-write Upstash key
  for the datastore holding accounts, billing and the corpus

Two things Sean should know rather than discover: a production KV token was pasted into a chat
transcript on 8 Aug and has not been rotated, and the Desktop package should not go anywhere
shared while it carries a token.

Delete `.env.local` after any script that needs it. Never paste a token into a chat.

---

## 11. What to do first

1. Read `HANDOVER.md`, `TREES.md` and this document.
2. Check every worktree's currency before touching anything (§2).
3. Ask Sean what he wants next. The candidates, roughly in order of value:
   - The **2.26.0 release** — sign, notarize, move the three gates. This is what carries a
     week's work to anyone else's machine, and it is the only thing blocking a beta.
   - The **arrival path** — the queue worker (§8). Removes Sean from the loop.
   - The **GUI vs terminal sweep** difference — unexplained and it makes the app worse than the
     CLI for the one job it exists to do.
   - The **UAD modal** handling — 42 of 47 withdrawals on Mac 2 were UAD.

Do not start by fixing symptoms. Almost every problem in the last week turned out to be one
level up from where it appeared, and the ones that took longest were the ones where a confident
wrong diagnosis got acted on. State what would have to be false for your answer to be wrong,
before you give it.
