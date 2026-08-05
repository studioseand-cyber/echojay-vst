# Distributing ejmap — where the pipeline lives

Proposed 5 Aug 2026. **Nothing here is built.** Scoping only.

The mapper opens the app, presses **Scan**, presses **Sweep All**, and walks
away. Later a review queue appears in the app with what the server could not
settle. No terminal, no keys, no Python.

> **The client measures. The server decides. The client asks what is still
> needed.** Everything requiring a credential moves server-side; the one piece
> that does not exist at all is the *return path*, and §6–8 are its design.

---

## 0. What already exists, measured

| | state | where |
|---|---|---|
| **Map upload** | real, over TLS, byte-identical to its artefact | `EjmapSend.h` |
| **Map-state read** | real `GET /api/params/maps?identities=…`, no auth | `MainComponent.h:11606` |
| **Endpoint config** | `config.json` → `upload_url` + `ingest_token`, refused if group/other-readable | `EjmapMouth.h:303` |
| **Proposal reader** | `ProposalSet::load(root, fp)` reads `proposals/<fp>.json` — **and already carries `category`** | `EjmapAssignment.h:125` |
| **Progress UI** | `progressBar` + `progressLabel`, laid out only while visible | `MainComponent.h:6915` |
| **Supervisor** | `fork()` + `execv()`, so the parent never touches AppKit | `EjmapSupervisor.h:139` |

Two of those are the whole proposal in miniature:

- **The client already has exactly one shape for talking to the server**: POST an
  artefact, GET a state. Neither stage 0.5 nor stage 2 needs a new shape.
- **`ProposalSet` already reads a per-fingerprint file that carries a category
  and a set of proposed semantics.** The client's *read* side for both stages is
  built. What is missing is the fetch.

And one thing that is genuinely missing, named up front because it is the real
blocker (§4): `EjmapSend.h`'s own scope note reads *"one mapper, this machine.
No queue, no per-tester tokens, no sign-in."* There is **one shared ingest
token**, and it is in your `config.json`.

---

## 1. Sweep All as a button

### 1.1 The button

Toolbar, after **Scan**, because that is the order the mapper works in:

```
[ Scan ] [ Sweep All ] [ Load… ] [ signal ] [ Arm ] …
```

`Sweep All` is enabled when a scan exists and categories are known. While
running it becomes **Stop**, and the existing `progressBar`/`progressLabel` show
`312 / 669 · UAD Pultec EQP-1A · ~4h20m left`. Both already exist and already
take no layout space when hidden.

### 1.2 One plugin per timer tick, not one long call

The CLI loop runs to completion inside one `callAsync`. That is fine when nobody
is watching; in the GUI it means Stop cannot be clicked between plugins.

> **Drive the loop from a `Timer`: one plugin per callback.** Between plugins the
> message loop is completely free, so the window repaints, Stop responds, and the
> mapper can cancel. Inside a plugin the sweeper already pumps
> (`runDispatchLoopUntil`), so the window stays alive there too — but the
> *guarantee* comes from the gap, not from the pump.

Stop is a flag read at the top of each tick. It stops **between** plugins, never
mid-sweep, so no half-written map exists — the same boundary the crash path
already relies on.

### 1.3 The relaunch, which the mapper must never see

The supervisor works from inside the process today — it `fork()`s and `execv()`s
before any GUI exists, so the parent links no AppKit. It only needs `--supervise`
on the command line, which a double-clicked `.app` will never have.

> **Make supervision the default for a GUI launch.** A launch that is not
> `--child` and carries no headless flag re-execs itself as the supervisor. Every
> existing headless flag is unaffected, because `runHeadlessCli` returns first.

Three details that make it invisible rather than merely automatic:

- **The parent must not take `instance.lock`.** It is claimed in `main()` after
  the supervisor branch returns, so the child claims it and the parent never
  does. Already correct; it needs a test so it stays correct.
- **Supervisor output goes to a file**, `supervisor.log` in the ledger root, not
  to a terminal nobody is looking at.
- **The restart is reported in the UI, not hidden.** The child already receives
  `--after-exit` and the status line already says *"RESTARTED after
  AudioUnit:… (signal:11)"*. On resume the sweep should say, in the mapper's
  words: *"UAD Pultec crashed. It has been set aside; carrying on from 313."*

When the supervisor does stop — three consecutive deaths with no progress — the
app must **stay open and say so**, rather than exiting. That is the one case
where the mapper has to know.

### 1.4 What the mapper sees when something goes wrong

Nothing, until the end. The eleven outcome classes all record and continue; the
circuit breaker is the single interruption and it already names its class. At the
end, one screen: *N mapped, N flagged, N set aside*, with the flagged list.

---

## 2. Stage 0.5 — categorisation moves to the server

You are right that this is the same shape as everything else, and there is a
bigger reason than credentials.

> **A category is a fact about a PRODUCT, not about a machine.** Waves CLA-76 is
> a compressor on every mapper's computer. Categorising it once, on the server,
> and serving the answer to everyone is not a workaround for key distribution —
> it is the correct place for it. The hundredth mapper's catalogue is ~95%
> already answered and costs nothing.

### 2.1 What the client sends

`POST /api/params/categorise` — the scan census, one row per scanned plugin:

```json
{"tester":"…","plugins":[
  {"name":"CLA-76 (m)","vendor":"Waves","format":"AudioUnit",
   "uid":"42f15d9","declared":"","version":"14.0.0"}]}
```

Every field is already in `scan-cache.xml`. ~1,819 rows ≈ 220 KB. **No parameter
data and no audio** — nothing is loaded to produce this.

> **It is still a list of the software on someone's computer.** That is personal
> information of a kind, and it should be said out loud in whatever the mapper
> agrees to, and stored keyed by product rather than by person wherever possible.

### 2.2 What the server does

1. Collapse to products (the `(m)`/`(s)` rule, server-side now).
2. Answer from cache for everything it knows — most of it, after the first
   mapper.
3. Queue the unknown remainder for the two-arm call, with your keys.
4. Escalations go to **your** review queue. The mapper never sees a disagreement.

### 2.3 What the client receives

The `categories.json` it already reads, unchanged in shape:

```json
{"products":{"cla-76|waves":{"category":"compressor","confidence":"high",
  "disposition":"sweep","refused_by_both":false}}}
```

A product the server has not decided yet comes back `"disposition":"pending"`,
and the sweep skips it — it is not lost, it resolves on the next refresh.

### 2.4 Offline

**Sweep All refuses to start without categories, and says why**: *"Connect once
so your plugins can be categorised. Without it the sweep would open 404 plugins
that have nothing to dial."*

That is a deliberate choice against the alternative. A missing category cannot
produce a wrong dial (§3 of the sweep proposal), so running blind is *safe* — it
is just wasteful: hours spent opening analysers and meters, and a corpus of maps
of spectrum analysers. Refusing is the better default; a hidden override can
exist for you.

---

## 3. Stage 2 — proposal moves to the server, and needs almost nothing new

This one is nearly free, because **the client already sends the input and already
reads the output.**

- The **input** to stage 2 is the map, and the client already POSTs it at submit.
  So the server can propose semantics *on ingest*. No new upload, no new gate.
- The **output** is `proposals/<fp>.json`, and `ProposalSet::load` already reads
  exactly that file and already carries both `category` and the per-index
  proposals.

> **One new endpoint: `GET /api/params/proposals?fp=…`** (batched, like the
> map-state read). The client writes the reply into `proposals/<fp>.json` and
> every existing reader works untouched — the wizard, the assign panel, the
> band flow.

### 3.1 Who reviews

**You do, server-side.** Stage 5 never ships. A mapper is a source of measured
evidence, not an adjudicator: the review pile is where the corpus's vocabulary
decisions get made, and those must not fork across machines.

That also settles the trust question — nothing a mapper does can promote a
semantic to `human-verified` except touching the control, which is already the
rule.

### 3.2 What this buys beyond credentials

A map submitted by any mapper is proposed against **the whole corpus**, with your
prompt version and your review history. Today a proposal depends on which laptop
ran the script.

---

## 4. The thing that does not exist: per-mapper identity

Everything above assumes the server can tell mappers apart. Today it cannot:
one shared `ingest_token`, sitting in your `config.json`.

Shipping that token to mappers means: any leak lets anyone write to the corpus,
and revoking it locks out everyone at once. It also makes "who mapped this"
unanswerable, which the provenance record currently claims to answer via
`tester.json` — **a name the mapper types, not an identity.**

What is needed, and it is not large:

- a **per-mapper token**, issued once, pasted once into a sign-in field, stored
  in `config.json` — which **already refuses to read a token whose file is
  readable by group or other**, so the storage rule is built;
- **server-side revocation**, so one leak is one mapper;
- provenance recorded from the token, not from a typed name.

**This blocks distribution and nothing else. It should be built first**, because
every endpoint below is otherwise built against an auth model that has to change.

---

## 5. The capture stage — free during the sweep, with one ordering rule

The sweep opens every editor anyway, so the capture costs nothing extra. Two
things make it not quite free.

### 5.1 Capture AFTER submit, never before

> **UAD Ampeg B15N hangs inside `cacheDisplayInRect` for 150+ seconds**, having
> loaded cleanly seconds earlier (`EjmapWatchdog.h`). The stuck thread is inside
> plugin code, there is nothing safe to unwind, and the watchdog's only move is
> to stop the process.

So the ordering is load → sweep → **submit** → capture. If capture runs first, a
hang costs the map that was otherwise finished. If it runs last, it costs only
the image, and the crash path already handles the rest: the map is on disk, the
worklist recomputes, the plugin is not offered again.

The capture gets its own watchdog scope, shorter than the load's. A plugin that
hangs in capture is recorded `capture_unavailable` and is not quarantined for
it — it maps perfectly well.

### 5.2 An empty capture is a failure, and it is already detected

`67de0d5` reports the fraction of non-background pixels precisely because a
capture that "succeeds" and returns an empty rectangle is the failure that
matters:

```
Eiosis AirEQ (nested JUCE, in-process)  2044x1400  52.6%  legible
API-550A     (Waves, bridged NSRemoteView) 632x1194  0.0%  EMPTY
```

**The client uploads a capture only when it is non-empty.** An empty one is
recorded as `capture_empty` and nothing is sent — a blank PNG in the corpus is
worse than no PNG, because it looks like evidence.

---

## 6. Bridged plugins — three routes, measured

### 6.1 How big it is

| | |
|---|---|
| sweepable products | 669 |
| of those, Waves | **210 (31%)** |
| Waves products shipping VST3 *in this scan* | **0** |
| sweepable products with any VST3 sibling | 300 (45%) |
| M9's earlier count of bridged AUs that are Waves | 604 of 622 |

So the blind spot is essentially Waves, it is 31% of what the sweep will open,
and the obvious fallback does not reach it.

### 6.2 Route 1 — the VST3 sibling. PROBED 5 Aug 2026: the cause is
architecture, and the route is dead anyway

> **Both halves measured with `ejmap-arch-probe` (`tests/ArchProbe.cpp`). The
> first half answered the question that was asked. The second half killed the
> route and corrected `67de0d5`'s attribution.**

**Why the scan gets nothing.** Every Waves shell on this machine is
**x86_64-only** — the VST3 shells and the AU shells alike, `WaveShell1-VST3 12.6`
dated January 2021. This machine is an M1 Pro and ejmap runs arm64. A VST3 must
be `dlopen`ed **in-process** and there is no bridge for it, so the load fails
before any factory is reached:

```
native arm64 :   0 descriptions in     9 ms
arch -x86_64 :  14 descriptions in 18,983 ms   (Abbey Road Studio 3, Nx …)
```

Not a scan gap, not licensing, not a JUCE limitation, not shell enumeration.
**"Bridged" is not a Waves property — it is an x86_64-only plugin in an arm64
host.** AU survives it by hosting out-of-process (the `NSRemoteView`); VST3
cannot, so it vanishes from the scan entirely.

**And the route is still dead**, because enumerating is only worth something if
the editor then captures. Loaded under Rosetta, a Waves VST3 comes up
**in-process** — no `NSRemoteView` anywhere in the tree:

```
- JUCEView_e713bae482ba613e            wantsLayer=YES  layer=yes
  - JuceInnerNSView_62ddac1ed097846e   wantsLayer=no   layer=yes
    - wvWavesV12_6_0_167_WavesView     wantsLayer=YES  layer=yes
capture : captured 1680x1228, non-background 0.0%
```

Same 0.0%. Reproduced on a second shell (`WaveShell2-VST3 12.1`, CLA EchoSphere,
782×1152, 0.0%).

> **THE EMPTY CAPTURE WAS NEVER CAUSED BY BRIDGING.** `67de0d5` measured
> API-550A at 0.0% *and* observed it was an `NSRemoteView`, and read the second
> as the cause of the first. A Waves panel that is demonstrably **not** remote
> reads 0.0% too. The cause is Waves' own rendering path — the shell ships a
> `default.metallib`, so it draws on the GPU, and `cacheDisplayInRect` captures
> the Core Graphics backing store, which for a GPU-drawn view is empty.
>
> The correlate is suggestive, not established: n=2. Eiosis AirEQ *links*
> OpenGL and captures fine at 52.6%, so "links a GPU framework" is not the
> predictor. What is established is the negative — hosting model is not the
> cause — and that is what matters here.

**So Rosetta buys nothing and costs something.** It would not fix capture, and
Waves plugins already map fine through their bridged AU form, so it does not fix
mapping either. It would drop `School EQ` (the one arm64-only component and the
one arm64-only VST3 on this machine). **Do not run ejmap under Rosetta.**

### 6.2b The old §6.2, superseded — a scan gap, cause unmeasured

A bridged AU whose product also ships a VST3 can be captured through the VST3
instead: same panel, in-process, no permission, no new API.

**The eight Waves VST3 shells are on this machine** —
`WaveShell1-VST3 12.6.vst3` and siblings, in `/Library/Audio/Plug-Ins/VST3/`.
The scan reaches every one of them and gets nothing:

```
no description for VST3 /Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 12.6.vst3
… eight of eight, scan-errors-20260804T105746-bc12.log
```

A shell exposes many plugins from one bundle, and JUCE's VST3 host is returning
zero descriptions from these. **Why is not measured.** It could be licensing, an
initialisation the shell needs, or a JUCE limitation.

> **This is the probe worth running first**, because it is worth more than
> capture: if those shells enumerate, 210 products stop being bridged for
> *mapping* as well as for capture — in-process editors, working
> `cacheDisplayInRect`, no TCC prompt, no OS floor. It costs one afternoon and
> the answer is a number, exactly like `67de0d5`.

### 6.3 Route 2 — ScreenCaptureKit. Available, unproven, and it has a price

`67de0d5` established that `CGWindowListCreateImage` is gone from this SDK and
that `screencapture` cannot reach another Space. It did not test the thing the
SDK points at instead. It is there:

```
SCScreenshotManager.h
+ captureImageWithFilter:configuration:completionHandler:   macos(14.0)
```

It goes through the **window server**, which is where a bridged editor's pixels
are actually composited — so it is the one route that can plausibly see an
`NSRemoteView`. The price is real and all of it lands on the mapper:

- a **Screen Recording permission prompt**, per machine, which is the most
  alarming permission macOS asks for;
- a **macOS 14 floor** against a deployment target of 11.0;
- the window must be **on screen and not minimised**;
- an async completion handler inside a loop that is otherwise synchronous.

> **Do not design around it until it is probed.** Same shape as `67de0d5`:
> capture API-550A, report the non-background fraction. If it reads 0.0% the
> question is closed for good; if it reads 50% the permission conversation is
> worth having.

### 6.4 Route 3 — accept a lower ceiling, which is the answer if 1 and 2 fail

And it is not a bad answer.

> **Vision is stage 4 — it runs only on what stages 2–3 could not settle.** The
> hold-out measured text-only two-arm agreement at 98.7% auto-accept precision
> over 598 controls. Bridged plugins lose the *tie-breaker*, not the pipeline.

A bridged plugin gets: swept, proposed from text, accepted where the arms agree,
and everything else queued for a human. That is a real ceiling and it should be
**recorded in the map**, not left implicit — so a later reader knows the
escalation rate on Waves is a property of the evidence, not of the plugins.

> **Do not call the field `unavailable_bridged`.** §6.2 measured that bridging
> is not the cause: a Waves panel captures empty whether it is remote or
> in-process. Naming it after the wrong cause would freeze the wrong diagnosis
> into the corpus, which is the `crash_on_load` mistake again — a field that
> asserts a cause the evidence does not carry.
>
> Record what was observed: `capture: "empty"`, with the measured
> non-background fraction and the pixel dimensions beside it. A later reader can
> then ask *why* against real numbers instead of inheriting a guess.

### 6.5 The manual screenshot — keep it, for you, hidden

Yes, and narrowly:

- **Never in a mapper's path.** Your judgement was right: nobody is dropping 200
  files into an inbox folder, and a product that asks them to is broken.
- **Kept as a power-user path behind a flag**, because you will work the residue
  and the residue is bridged escalations that text could not settle. One
  plugin, one screenshot, one answer — not a batch discipline.
- **It is the residue of the residue.** How big is measurable once the sweep
  runs and not before, so no number is offered here.

---

## 7. The return path — what the server sends back

This is the piece that does not exist. Today the client submits and forgets.

### 7.1 What the client sends, and when

| artefact | endpoint | when |
|---|---|---|
| the controls-only map | `POST /api/params/ejmap` *(exists)* | at submit, per plugin |
| the editor capture, **only if non-empty** | `POST /api/params/capture?fp=…` | after submit, per plugin |

The capture is a **separate request on purpose**. The map's transport is Option A
— the TLS plaintext *is* the artefact, byte for byte — and putting a PNG inside
that body would end byte-truth for the one artefact that has it.

### 7.2 What the server does, and when

**Per map, on ingest — not batched at the end of the sweep.** A sweep runs for
hours, so per-map means the early maps are proposed long before the mapper
finishes, and the queue is populated by the time they look at it. Batching would
idle the server for four hours and then spike.

1. ingest and gate the map (exists)
2. **stage 2**: two-arm proposal over the controls
3. **stage 3**: the mechanical gates — agreement, confidence, unit family, no
   duplicate semantic
4. **stage 4**: the vision pass, on escalations only, where a capture exists
5. what still cannot be settled becomes a **card in that mapper's queue**

### 7.3 What comes back

```
GET /api/params/queue           -> { "count": 40, "cards": [ … ] }
GET /api/params/queue?count=1   -> { "count": 40 }
```

A card is what `review.py` already renders — plugin, control, evidence, both
arms' answers, and why it escalated — as JSON instead of terminal text.

**Scoped to that mapper's plugins.** A mapper answers questions about software
they own and can open. Yours is the same endpoint with a wider scope.

### 7.4 What the answers do

```
POST /api/params/decisions   [{fp, index, outcome, semantic, source, trust}]
```

The server writes them into the map, re-runs the gate, and the map becomes
dialable. The client's queue count drops.

**Two rules that must survive the move into the app**, because both already
exist and both are easy to lose in a UI:

- **Reading is not verifying.** Accepting or correcting by reading records
  `human-corrected` and leaves trust at `llm-classified`. Only touching the
  control produces `human-verified`. A card in an app makes answering *feel*
  more authoritative than it is.
- **A mapper's answer settles their map, not the corpus.** Promoting one answer
  to corpus-wide truth stays yours. Otherwise the corpus is writable by anyone
  with the app — the same reasoning that made a single uncorroborated wrongness
  report queue rather than act.

---

## 8. The queue as a visible state, and one interface for both of us

### 8.1 A count in the toolbar

The send backlog already sits at the right-hand end of the toolbar *"where it is
in view for the whole session rather than for one keystroke"* — the comment is
already in `resized()`. The queue count goes beside it:

```
                                     40 questions waiting   ·   3 maps to send
```

Refreshed on app start, on sweep completion, and on a light poll while the app
is open. **Visible, not discovered** — which was the requirement.

### 8.2 The same interface for your residue

Yes, and it should be a requirement rather than a nice-to-have:

> **If the review queue is good enough for a mapper it is good enough for you,
> and maintaining `review.py` alongside it is how the two answers drift.**

`review.py` becomes reference and local tooling, exactly as `categorise.py` and
`propose.py` do. Your view is the same cards with a wider scope and the extra
dispositions you need — the cohort collapse, the band grouping, `[t]`
verify-by-touch — which are additions to one interface, not a second one.

The one thing your view needs that a mapper's does not is **the vocabulary
decisions**: `no_dial_set` products, semantic proposals that do not fit the
eleven categories. Those are corpus decisions and they stay on your side of the
scope filter.

---

## 9. The whole pipeline, once it is distributable

| stage | runs | client sends | client receives |
|---|---|---|---|
| 0 scan | client | — | — |
| **0.5 categorise** | **server** | scan census | category + disposition per product |
| 1 sweep | client | — | — |
| submit | client | the map (exists) | accepted / rejected (exists) |
| **capture** | client, **after submit** | the panel PNG, only if non-empty | — |
| **2 propose** | **server**, per map on ingest | nothing new | — |
| **3 gate** | server | — | — |
| **4 vision** | **server**, escalations only, where a capture exists | — | — |
| **review** | **the app**, mapper and you, one interface | decisions | cards + a count |
| dial | EchoJay client | — | maps (exists) |

Five endpoints, two of them already built:

```
POST /api/params/ejmap        submit a map                 EXISTS
GET  /api/params/maps         map state                    EXISTS
POST /api/auth/mapper         token -> identity            NEW  (blocks the rest)
POST /api/params/categorise   scan census -> categories    NEW
POST /api/params/capture      fp + PNG                     NEW
GET  /api/params/queue        what still needs a human     NEW  (the return path)
POST /api/params/decisions    answers -> written into maps NEW
```

`GET /api/params/proposals` is **not** in that list any more. Once stage 2 runs
server-side per map and everything settled is written straight into the map, the
only thing the client needs back is what is *unsettled* — which is the queue. A
separate proposals fetch would be a second way to say the same thing.

**No Python ships. No keys ship. No screenshots are filed by hand. The client
gains a button, a queue and a count.**

### 9.1 The mapper's whole loop

```
Scan  ->  Sweep All  ->  (walk away)  ->  "40 questions waiting"  ->  answer them
```

### 9.2 Yours

The same four steps, plus the scope filter that shows you every mapper's
residue, the vocabulary decisions, and the bridged escalations you will work by
hand with a screenshot.

---

## 10. Build order

**Two probes first, because both are cheap and both change what gets designed.**

~~0a. The WaveShell probe~~ **DONE 5 Aug 2026.** The cause is architecture; the
    route is dead anyway, because an in-process Waves VST3 still captures at
    0.0%. See §6.2.

~~0b. The ScreenCaptureKit probe~~ **NOT RUN, deliberately.** Declined on cost —
    a Screen Recording prompt on every mapper's machine and a macOS 14 floor
    against an 11.0 target — and §6.2 strengthens the decision rather than
    weakening it: SCK is now the *only* remaining route, since it is the only
    one that reads what the window server composited rather than what a view
    will redraw. Route 3 is the answer.

So:

1. **`Sweep All` button** — timer-driven loop, Stop, progress, self-supervision,
   and capture-after-submit. **No server work, and it changes your week.** Build
   this first even though identity blocks everything else.
2. **Per-mapper tokens** (§4). Every endpoint below is otherwise built against an
   auth model that has to change.
3. **`POST /api/params/categorise`** + the client fetch. Port `categorise.py`
   server-side unchanged — prompt, gate and dispositions are already proven
   against 103/103 hand answers.
4. **Stage 2 on ingest** + `POST /api/params/capture`. Port `propose.py`.
5. **The return path**: `GET /api/params/queue`, the toolbar count, the card UI,
   `POST /api/params/decisions`.
6. Retire `categorise.py`, `propose.py` and `review.py` from anyone's path.
   They stay as the reference implementation and as your local tools.

Step 1 is independent of everything else. Steps 3–5 are the product.

---

## 11. Decided 5 Aug 2026

1. **Sign-in: a per-mapper token, pasted once.** Enough for five people, and the
   only part of §4 that blocks distribution. Accounts when there is a reason for
   accounts.
2. **Proposal on ingest**, not on demand. The sweep's whole shape is walk away
   and come back to answers; on demand means coming back to nothing.
3. **The census: mappers are told it is sent, and storage is keyed by PRODUCT,
   not by person.** See §11.1 — including what the server retains, because a
   mapper asking "what do you do with the list of my plugins" should get an
   answer that is already written down.
4. **Offline Sweep All refuses**, with the reason on screen. A hidden override
   for the owner.
5. ~~The two probes~~ **DONE.** The WaveShell cause is architecture and the route
   is dead anyway (§6.2). The ceiling is accepted: bridged plugins are swept,
   proposed from text, and escalate to the queue. No vision.
6. ~~Screen Recording~~ **DECLINED.** Not worth an alarming permission on every
   mapper's machine and a macOS 14 floor, for 31% of a stage that only runs on
   what text could not settle.

### 11.1 What the server keeps, and what it does not

This is the paragraph a mapper gets shown, so it is written before the endpoint
is built rather than after someone asks.

| | |
|---|---|
| **Sent** | plugin name, vendor, format, uid, version — for every installed plug-in |
| **Not sent** | no parameters, no audio, no presets, no project data, no file paths beyond the plug-in's own identifier. Nothing is loaded to produce it |
| **Retained, keyed by PRODUCT** | one row per `(name, vendor)`: its category, both arms' answers, the prompt hash, the date. **This is the cache and it is the point** — it is what makes the hundredth mapper's catalogue free, and it contains no reference to any person |
| **Retained, keyed by MAPPER** | nothing from the census. The token identifies who submitted a *map*, because provenance on a corpus entry has to be answerable. It does not record what they own |
| **Discarded** | the association between a mapper and the products in their census, once the unknown ones have been queued. The server needs *"CLA-76 is a compressor"* and never *"this person owns CLA-76"* |

The last row is the rule the other rows follow from, and it is the right shape
independently of privacy: a category cache keyed by person would answer the same
question once per mapper instead of once ever.
