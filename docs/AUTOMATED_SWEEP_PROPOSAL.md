# Stage 0.5 (categorise) and the automated sweep — proposal

Proposed 5 Aug 2026. **Nothing here is built.** Everything below that carries a
number was measured today; the numbers are marked with how.

---

## 0. What I measured before proposing

| question | answer | how |
|---|---|---|
| Is a category required to write a map? | **No.** Nothing in ejmap rejects an empty one | grep: no `category.isEmpty()` guard exists |
| Does the client read the category at dial time? | **Never.** `Source/` has no consumer | grep across the plugin |
| Does the category restrict the proposer? | **No.** `VOCAB` is the *union* of every category's dial set | `tools/propose/evidence.py:12` |
| So what does it do? | **One line of prompt context**: `category: X` | `evidence.py:105` |
| Is param count available pre-load? | **No** — it needs an instance | `scan-cache.xml` attributes |
| Catalogue size | 1,819 scan rows → **1,325** distinct (name, vendor) → **1,073** after collapsing `(m)/(s)/NxN` variants | `scan-cache.xml` |

**The first three together are the answer to your question 1**, before any model
runs: a wrong category cannot produce a wrong dial. It cannot even produce a
semantic outside the vocabulary, because the vocabulary is not category-scoped.
It nudges the prompt. The accept gate — two arms agree, both confident, unit
family holds, no duplicate claim — is untouched by it.

So the sweep *could* proceed with no category at all. Stage 0.5 is still worth
building, for a better reason than necessity: it decides **what not to load**.

---

## 1. Stage 0.5 — categorise the catalogue in one pass

### 1.1 What is actually available pre-load

Your list was name, vendor, format, param count. **Param count is not available**
— it requires instantiation, which is the thing this pass exists to avoid. What
the scan cache does carry per row:

```
name  manufacturer  version  uid  format  isInstrument  isShell
numInputs  numOutputs  hasARAExtension  category
```

Two of those matter and one is a real find:

- **`category` is the format's own subcategory string** — `Fx|Dynamics`,
  `Fx|Channel Strip`, `Fx|Reverb`. Free, vendor-declared, no model needed.
- **`numInputs`/`numOutputs` are useless pre-load**: 1,534 of 1,819 read `0`.

Coverage of the free signal, measured:

| | rows | products |
|---|---|---|
| declares a subcategory | 500 / 1819 | 499 / 1325 |
| after inheriting across AU↔VST3 siblings | **993 / 1819 (55%)** | 499 / 1325 (38%) |

Every VST3 declares one; **no AudioUnit does**. So the AU half can inherit from
its VST3 sibling by (name, vendor) — 493 rows for free. The products with
nothing at all are 826, and **789 of them (96%) are Waves (604) and Plugin
Alliance (185)**, both AU-only here.

The subcategory does not decide the answer — `Fx|Dynamics` spans compressor,
limiter, gate, de-esser and transient_shaper — so it goes into the prompt as a
constraint, not as a verdict.

### 1.2 The run, and what it measured

Same shape as the semantic hold-out: `claude-opus-5` and `gpt-5.5`, same system
prompt, neither sees the other, 25 plugins per request.

**Truth run — the 40 plugins you categorised by hand.** Run twice, once without
vendor and once with (the first pass read `identity.manufacturer`, which does
not exist; the key is `vendor`):

| | without vendor | with vendor |
|---|---|---|
| agreed | 39/40 (97.5%) | 39/40 (97.5%) |
| agreements matching your answer | 37/39 (94.9%) | 37/39 (94.9%) |
| **both-confident agreements matching your answer** | **34/34 = 100%** | **33/33 = 100%** |

**67 both-confident agreements across the two runs, zero disagreeing with your
hand answer.** Every row it got "wrong" had at least one arm hedge:

```
AVOX PUNCH          you=limiter     opus=transient_shaper(low)  gpt=compressor(low)
UAD bx_refinement   you=saturation  opus=eq(low)                gpt=eq(high)
SPL Vitalizer MK2-T you=saturation  opus=eq(low)                gpt=eq(high)
AVOX WARM           you=saturation  opus=saturation(low)        gpt=saturation(high)  ✓
SSL Blitzer         you=compressor  opus=compressor(low)        gpt=compressor(high)  ✓
Aphex Exciter (m/s) you=saturation  opus=saturation(low)        gpt=saturation(high)  ✓
```

Your guess was right about *which* ones: the exciter/enhancer class, saturation
versus eq. Note the shape — **the confidence gate is conservative, not
accurate-on-average**: 4 of those 6 hedged rows were correct and got held back
anyway. It costs coverage, not correctness. Same finding as unit-absence.

**Volume run — 150 random uncategorised products**, weighted as the catalogue is
(71 Waves, 22 Plugin Alliance, 20 UAD, 16 Melda):

| bucket | n | % | projected on 1,073 |
|---|---|---|---|
| agreed real category, both confident | 80 | 53.3% | 572 |
| agreed real category, one arm hedged | 13 | 8.7% | 93 |
| agreed `none` — a processor, no dial set fits | 41 | 27.3% | 293 |
| agreed `not_a_processor` | 9 | 6.0% | 64 |
| **disagreed** | **7** | **4.7%** | **~50** |

The subcategory being present made no measurable difference to agreement (94%
with, 96% without) — the models know these products.

### 1.3 The disposition I propose

| bucket | what the sweep does |
|---|---|
| agreed + both confident | sweep with that category |
| agreed + one arm hedged | **sweep anyway**, record `confidence: low` |
| agreed `none` | **never load.** Not unmappable — see below |
| agreed `not_a_processor` | **never load**, propose an unmappable mark |
| disagreed | **the only thing that reaches you: ~50 rows** |

Accepting hedged agreements is the argument that follows from §0: the category
is a prompt nudge over an unrestricted vocabulary, so a hedged-but-agreed
category is strictly better than the `unknown` the proposer already tolerates,
and its worst case is a slightly worse nudge. It is recorded as low-confidence
so it can be re-derived later without re-running anything.

If you would rather see them, hedged real categories add 13/150 → **~143 to
review instead of ~50**. My recommendation is ~50, with the hedged ones written
to a list you can read in one sitting afterwards rather than in the loop.

### 1.4 The unmappable question — yes, but only for one of the two refusals

**They must not be one bucket, and this is the main design output of the run.**

- **`not_a_processor`** — analysers, meters, matrix routers, generators. Nothing
  to dial, ever. Measured: 9/150 (6%), 8 of them both-confident. Every one is
  unambiguous: `PAZ-Analyzer`, `MAnalyzer`, `WLM Meter`, `Insight 2`,
  `MChannelMatrix`, `MStereoScope`, `EMO-Generator`. **A confident agreed
  `not_a_processor` should propose an unmappable mark** — a proposal you accept
  in a batch, not an auto-write, because `unmappable` is forward-carried and
  deliberately hard to undo.

- **`none`** — a real processor no category fits: filters, modulation, imaging,
  pitch. Measured 41/150 (27%), including `B360`, `MetaFilter`,
  `Moog Multimode Filter`, `OVox`, `MRhythmizer`. **These must NOT be marked
  unmappable.** `none` is a statement about *your vocabulary*, not about the
  plugin — the same eleven-category gap queue item 18 already records for
  Auto-Tune and tuners. Marking them unmappable would bury ~293 products behind
  a decision that reads as "this plugin cannot be mapped" when what happened is
  "EchoJay has no dial set for imagers yet."

  They get their own state — `no_category`, out of scope, kept forward — and
  they are the ranked list that says which dial set to add next.

**One correction to your message.** `marks.json` holds **zero** unmappable
entries today (`{"issues":{},"unmappable":{}}`). So there is no hand-marked set
to validate the refusals against; B360 and Sub Align are named in the queue prose,
not recorded as marks. The `not_a_processor` bucket is a *proposal to check*, not
a rule confirmed against your answers — unlike the category itself, which has 40.

### 1.5 Cost

Measured on a real 25-plugin batch: Opus 1,607 in / 1,970 out; GPT-5.5 1,021 in
/ 2,559 out; 22 s and 39 s.

| scope | batches | Opus | GPT-5.5 | total | wall clock |
|---|---|---|---|---|---|
| 1,073 collapsed products | 43 | $2.46 | $1.78 | **$4.25** | ~5 min |
| 1,325 distinct products | 53 | $3.04 | $2.20 | $5.23 | ~6 min |
| 1,819 scan rows | 73 | $4.18 | $3.03 | $7.21 | ~8 min |

**Categorise the 1,073 collapsed set and fan the answer out to variants.** A
`(m)`/`(s)` pair is one product; the prompt already says a suffix never changes
the category, and paying twice to be told so twice is waste. Output is keyed by
(name, vendor) and applied to every row that collapses to it.

The output is `~/Library/ejmap/categories.json`, one entry per collapsed
product, carrying both arms' answers, both confidences, the declared
subcategory, and the disposition. Resume-by-presence at the batch level — and
per the 529 finding, **a failed arm writes nothing**.

---

## 2. The automated sweep

### 2.1 The loop

It is `--selftest-controlsonly` with the assertions removed and a worklist
around it. That path already does exactly the four steps, end to end, and
already writes a real map: load → `pickCategory` → controls row → `space`,
`space` → `submit`.

```
ejmap --sweep [--limit N] [--dry-run]

  for each row the worklist offers:
      category = categories.json[product]
      if disposition is never-load:            record, next
      if quarantined:                          record, next
      load  ->  sweep controls  ->  submit  ->  record, next
```

Progress goes to stdout and to `sweep-<run>.jsonl`, one line per plugin, written
before the next load starts — so a crash loses the plugin in flight and nothing
else.

### 2.2 Question 2 — what counts as "cannot proceed"

Your instinct is right, and the code is already most of the way there: **an
unbuildable control is excluded per control today**, and the map is written with
the rest. Measured over your 40 maps, 600 of 882 params became named controls —
**32% already fail to sweep and it has never been an interruption.**

| what happens | proposed | why |
|---|---|---|
| crash on load | **record + continue** | already handled: the retry rule quarantines at 3, the supervisor relaunches, the worklist skips |
| already quarantined | **record + continue** | counted in the run summary, not offered |
| licence refused | **record + continue**, but see the circuit breaker | your SSL failures were iLok, not capture |
| editor never becomes ready (20 s) | **record + continue** | the watchdog already writes a row and applies its own rule |
| some controls unbuildable / text liars | **continue silently** | the normal case — 32% of params — already recorded as skips in the map |
| **sweeps zero controls** | **record + FLAG as an issue, continue** | submit correctly refuses an empty map. Measured 2/40 (Cenozoix 0/99, CLA-76 (m) 0/9) → ~5%, ~50 plugins |
| submit/upload fails | **record + continue** | the map is written locally first; upload is a separate queue |

**Nothing stops and asks, with one exception.**

> **The circuit breaker.** Stop after **10 consecutive failures of the same
> class**. Ten licence refusals in a row is not ten plugins, it is one logged-out
> iLok — and a run that quietly records 400 licence failures overnight has
> produced a worthless night and a misleading ledger. This is the one case where
> "record and continue" records a fact about your machine as if it were a fact
> about 400 plugins.

That is the only interruption I would build. It offers your three options —
skip / flag / stop — and everything else takes `flag` automatically without
asking.

### 2.3 Crash survival

Already true, and worth stating explicitly because it is why this is safe to run
overnight:

- `beginLoad` stakes `inflight.json` before control passes to plugin code.
- The process dies. The supervisor relaunches.
- `recoverFromCrash` writes the death row and applies the retry rule.
- The worklist recomputes from `maps/` and `map-state.json` — **what is already
  mapped is never offered again**, so "carry on from where it stopped" needs no
  cursor and no resume file. Resume-by-presence, the same shape as the proposer.

The one thing to add: `--sweep` must pass through the supervisor so the relaunch
happens, and must re-read `categories.json` rather than holding it in memory.

### 2.4 What the run produces

```
SWEEP  312/1073   ~4h20m remaining
  now: UAD Pultec EQP-1A  (eq, both-confident)          swept 14 controls
  done   287 mapped   11 swept nothing   9 died   5 licence
```

and a summary at the end that is the review list: every flag, every zero-control
plugin, every quarantine, with the reason. One list, read once, after.

---

## 3. Build order

1. **Stage 0.5** — `tools/propose/categorise.py`, the batch runner and
   `categories.json`. Gate: re-run the 40-plugin truth set and assert
   both-confident agreement still matches your hand answers.
2. **The unmappable proposal list** — `not_a_processor` written as a *proposal*
   file, accepted in a batch, never auto-written to `marks.json`.
3. **`--sweep`** — the loop, the recorder, the circuit breaker.
4. **Acceptance tests, before it touches the catalogue** (the `--selftest`
   finding: two real defects came out of building the test first):
   - a plugin that sweeps nothing is flagged and the run continues
   - a plugin that dies is quarantined by the retry rule and the run continues
     on relaunch, and does not re-offer what is already mapped
   - 10 consecutive same-class failures stop the run
   - a `none` product is never loaded

---

## 4. What I need from you

1. **Hedged agreements: sweep them (~50 to review) or hold them (~143)?**
   Recommendation: sweep, list them afterwards.
2. **`not_a_processor` → an unmappable *proposal*, accepted in a batch.** Confirm
   that is the right weight, given there is no hand-marked set to check it
   against.
3. **`none` gets its own state and is not unmappable.** This is the one I would
   push back on if you disagree: ~293 products, and the list is the argument for
   which dial set to add next.
