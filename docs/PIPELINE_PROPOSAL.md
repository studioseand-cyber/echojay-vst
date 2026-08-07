# The semantic pipeline — build proposal

Proposed 4 Aug 2026, after the hold-out. **Nothing here is built.** This is scoping:
the stages, what is reused against what is new, the order, and what to build first.

## What the measurement settled

33 plugins, 125 human-confirmed params, two arms prompted identically (`claude-opus-5`
in a clean context, `gpt-5.5`), neither shown the other's answer or the truth.

- **Agreement predicts correctness.** Both agree + both confident: 75/125 accepted,
  **74/75 = 98.7%** correct, against 94.4% for one arm unconditional.

> **Qualified 7 Aug 2026:** measured against hand answers that are PARTLY MODEL-SOURCED -- the user has since said some of the 266 review decisions came from pasting cards into a third model, so the standard is not fully independent of what it scores; the direction holds because the errors were adjudicated by the unit-family rule over measured evidence, not by either party's opinion.

- **End to end 97.8%** — 90 written correct, 2 wrong, 92 written, 27 to review.
- **Unit-absence costs coverage, not accuracy.** Accuracy is flat across the split;
  auto-accept rate drops 83.7% → 44.7%. Stage 3 buys coverage back, not correctness.
- **The unit-family rule found the two wrong rows in the truth set** and nothing else.

Two design consequences, which the stage list below follows from:

1. **Only the sweep needs the plugin.** Proposing and vision-reading both run off
   `maps/` and PNGs on disk. So the catalogue costs exactly one load per plugin.
2. **The review pile is the deliverable cost.** ~22% of ~4,900 params at catalogue
   scale ≈ **1,100 rows**. Every decision below either shrinks that or protects the
   ~2,950 rows written without you.

---

## Stage 0 — the scan

### Correction: three of the four gaps are already closed

Reading `PluginScanner.cpp` and `EjmapScanProgress.h` before scoping this:

| believed missing | actually |
|---|---|
| cannot resume | **`ScanProgress` already does this** — one appended, flushed JSON line per probed bundle, disk-stamped, staleness-checked, refuse-and-rescan on an unreadable file. Built because three hanging bundles cost three full rescans. |
| the whole scan is slow and dangerous | **Only VST3 is.** `scanAudioUnits` reads registry metadata via `buildCensus()` — 1376 components in ~60 ms, and it never hands control to plugin code, so it cannot hang. The 4.5 minutes and the crash risk are VST3 opening modules for 646 of 861 bundles. |
| blocks the message thread | **Deliberate, with the reason in the code**: moving `scanVST3` to a worker changes which thread third-party module entry points run on — a scan-behaviour change, not a UI change. It pumps the loop every 50 ms instead so the window repaints. |

So stage 0 is **not** "build resume". It is a much smaller job.

### What is actually missing

- **Per-item timing.** The progress callback carries `(phase, done, total, id)` and no
  duration. Nothing can name the slow bundle afterwards. This is a *field on a line that
  is already being appended*, not a mechanism.
- **`scan-cache.xml` is written once, on completion** (`saveScanCache()` after
  `scanner.scan()` returns). A kill mid-run therefore loses the serialization but **not**
  the work — the next run restores probed bundles from `ScanProgress`. Worth making
  incremental anyway, at a checkpoint interval, so the visible artefact matches the
  journal.
- **No worklist.** Nothing turns the scan result into "what still needs doing".
- **Marks are not consulted** by anything batch-shaped.

### Acceptance test — sharper than "it resumes"

Killing a scan mid-run and having it resume **already passes today** on the path that
matters. So the proof has to be the thing that is not already true:

> A scan killed mid-run, restarted, must (a) complete measurably faster, (b) re-probe
> **only** bundles whose disk stamp moved, and (c) name the five slowest bundles with
> their durations. Assert all three, not just that it finished.

### What stage 0 must produce for stages 1–5

One artefact: **the worklist** — every plugin id in the catalogue with its status, so a
batch pass can be described as a filter rather than as a list of rows you clicked.

```
plugin_id · name · vendor · version · format
mapped(format|uid|version) · unmappable(format|uid) · issue(format|uid|version)
swept · proposed · needs_screenshot · last_scan_ms
```

### Marks, and the sentence that makes the catalogue tractable

`EjmapMarks.h` already keys the two marks differently, on purpose, and that difference is
exactly what a batch pass needs:

- **`unmappable` is keyed on the PRODUCT** (`format|uid`) — a decision that Auto-Key is
  not a mixing processor should survive a vendor update. A batch pass **skips these
  permanently**.
- **`issue` is keyed on the BUILD** (`format|uid|version`) — "bands would not infer" is a
  statement about the parameters *this version* exposes. A batch pass **surfaces these
  and does not silently retry**, and a new version clears the mark automatically because
  the key contains the version. Nothing to build for that; it falls out of the key shape.

**Marks are advisory, never a lock** — the header is explicit that a greyed row must stay
loadable. So the batch honours them *by default* and `--include-flagged` overrides,
rather than the mark gating anything.

Target invocation:

```
ejmap --sweep-worklist          # not mapped, not unmappable, not flagged
      --include-flagged         # opt back in to issue-marked builds
      --limit N --resume
```

---

## Stage 1 — control sweep, finishing with controls only

Existing wizard flow. One change: **finishing a map with controls only must be a
first-class terminal state, not deferred work.** An unresolved Tier 1 row is now *about
to be proposed*, not abandoned.

**Review screen.** Today the strip counts unresolved rows as outstanding, which is what
makes leaving them feel like quitting. Proposed: a third row state alongside
resolved/deferred — `to-propose` — and the strip reads *"12 controls, 4 to propose, 0
deferred"*. Deferred keeps its current meaning: *you* looked and could not decide.

**Structural gate.** It must accept a map with `params: {}` as complete. Related and
worth doing at the same time: **`accepted_groups: 0` is currently indistinguishable from
"never attempted"** (queue item 1). Once rows are routinely left for proposal, that
ambiguity stops being cosmetic — the gate cannot tell a controls-only map from a failed
inference. Persisting `strideNote` and the touched indices resolves both.

---

## Stage 2 — propose (offline, over `maps/`)

Runs over `~/Library/ejmap/maps/*.json`. **Never loads a plugin.** Both arms, prompted
identically, neither shown the other's answer.

**Reuse, not build:** `proposals/<fp>.json` already exists and `ProposalSet::load`
already reads it into the assignment panel —
`{category, params:[{index, kind, confidence, reason}]}`, where `kind` is the semantic.
The classifier writes that exact shape. The review surface is therefore partly built.

**Recording.** `trust: "llm-classified"` already exists in the vocabulary and
`EjmapExposure` already sorts `human-verified > setread > llm-classified > unknown`.
New: **`semantic_source`** on the param entry — `method` is taken, and it means how the
*index* was found (gettext/setread/human-typed), not where the *semantic* came from.

```
semantic_source: "model-proposed"            two arms agreed, both confident
                 "model-proposed+panel"      settled by the vision stage
                 "human-confirmed"           reviewed and accepted, not touched
                 "human-corrected"           reviewed and changed
                 "human-verified"            verified by touch
```

The pipeline may never write `human-verified`. Worth checking on the way that the
readback/verify path cannot silently promote it either.

**Resumability** is the file layout: one file per fingerprint, presence means done. No
separate mechanism.

**Runs over maps you already made** — this is the point of putting it offline. The 40
maps on disk today are proposable tonight.

---

## Stage 3 — vision on the residue

**When: capture during the sweep, classify later.** The measurement makes this decision
rather than leaving it to argument. The *capture* needs the editor open; the *vision
call* does not need the plugin at all, because the PNG is on disk. So there is no batch
reload pass, and the design session's argument — a reload costs 0.5–1.6 s and is where
Drawmer, MConvolutionMB and MLoudnessAnalyzer died — is not merely honoured but made
moot.

`captureHostedEditor` already exists (67de0d5) as a probe. Promote it: one
`cacheDisplayInRect` after the editor settles, write `screenshots/<fp>.png`, and
**persist the measured non-background fraction on the map**.

> **The PNG existing must never mean "captured."** A bridged plugin returns an
> empty rectangle and the capture still writes a file -- 13 KB of flat
> background, indistinguishable *by presence* from a real 2 MB editor. The
> measured fraction is the record; the file is only the payload. This is the
> same shape as the 529 defect that made `tools/propose` record 11 transient
> failures as completed work: a failure path writing the artefact a success
> path writes. Anything keyed on presence has to be checked for it.

**That fraction is the blind-spot criterion — not the vendor.** Empty capture caught 10
plugins in the hold-out: 8 Waves *plus FG-X 2 (Slate Digital) and spiff (oeksound)*.
Gating on `vendor == Waves` would have missed two and will miss more.

Measured: settles **18 of the 26 rows it can see (69%)** at 88.9% correct.

---

## Stage 4 — manual screenshot for empty-capture plugins

As designed in the previous session, with the trigger changed to the measured criterion.

- `~/Library/ejmap/screenshots/inbox/` accepts whatever macOS names the file. **No
  rename** — a fingerprint is 64 hex characters and you would be typing it mid-map.
- **`K` at map time** claims the newest shot from the inbox (or the Desktop) and files it
  under the fingerprint, while the editor is still open.
- **`--needs-screenshots`** lists fingerprint, plugin, and *the specific unit-less
  controls that must be legible* — the same query that produced the 49/76 split.

The four verification checks, all cheap:

1. **Dimensions** against the editor size ejmap already recorded, at integer scale.
2. **Timestamp** against the load — a shot older than the session is not this plugin.
3. **The model states what it sees before answering** — it named the panel correctly on
   every capturable plugin in the hold-out.
4. **Rejection if it cannot read the named controls** — this one fired for real: the
   model declared 3 controls it could not locate rather than guessing.

---

## Stage 5 — review

### The problem to design against

A list of confident claims invites blind acceptance, and blind acceptance launders
`llm-classified` into `human-verified` one keystroke at a time. So:

**Reading is not touching, and acceptance must not upgrade trust.**

| you do | semantic_source | trust |
|---|---|---|
| accept as-is | `human-confirmed` | **stays `llm-classified`** |
| correct it | `human-corrected` | `human-verified` |
| verify by touch | `human-verified` | `human-verified` |

Accepting a proposal records that a human *looked*, which is real and worth recording,
and is not the same claim as having moved the control. Only the two gestures that involve
your hand produce `human-verified`.

### What a row shows

Evidence first, claim second — the ordering matters:

```
UAD Vertigo VSM-3            compressor            [3 of 3 remaining]

  InptGain     range null   span -10..+10   unit (none declared)   21 anchors

  Opus  input_db  low        GPT  input_db  high
  ESCALATED: one arm declined
  panel: "Input section knob, scale engraved -10 ... +10; Clip LED ladder"  -> input_db

  [enter] accept    [1-9] pick another    [t] verify by touch    [d] defer
```

Declines are shown as declines, never collapsed into a single tidy answer. Sorted by
**semantic then category**, not by plugin — the throughput win is deciding twenty
`input_db`-vs-`drive` calls in a row.

### Corrections are a dataset, not a fix

`EjmapAssignment.h` already states this for the W-capture mismatch case
(*"MISMATCHES ARE A DATASET, not a burial"* → `misclassified-<run>.jsonl`). A correction
at review time is the same shape and should land in the same file: fingerprint, semantic,
what each arm proposed, what the panel read, what you chose. That is the labelled set for
tuning the proposal prompt, arriving free.

---

## What must be true of it

**The unit-family check runs on every accepted proposal, whatever its source.**
`unitFamilyConflicts` already sits beside `duplicateIndexConflicts` on both the review
screen and the submit path — one implementation, because a rule that exists twice is two
rules. The proposal path calls the same one. Evidence it is the right backstop: it finds
**exactly the 2 contradicting rows in 125**, including the ones a model got wrong
(HG-2 `output_db` on a `pct` control is precisely what a model proposed).

**Nothing reaches the server without passing the gate a hand map passes.** Same gate, no
proposal-specific exemption.

**Readback on a promoted param — the honest answer.** Verifying needs a live instance;
the proposal stage is offline. So a model-proposed param **carries no readback evidence,
and must record that as absent rather than as failed.** This project already has the
rule: *absent-key means unavailable*, not zero. Concretely:

- auto-accepted params ship with `readback: not_attempted` and `trust: llm-classified`
- the server must not read absent readback as failed readback
- the ~22% you review are the rows where a plugin can be loaded anyway, so **verification
  happens there, for free**, on exactly the rows least certain to begin with

That asymmetry is the right way round: the confident 60% ship unverified-but-flagged, the
uncertain 22% get a live instance.

**Every stage resumes.** Per-fingerprint files throughout, and the append-and-flush
ledger pattern already proven by `ScanProgress`. A crash costs the current plugin.

---

## Build order

Ordered so you can use it before all of it exists.

| # | build | why here | new vs reused |
|---|---|---|---|
| **1** | **Stage 2 over existing maps** | Zero risk, no C++, no plugin loads — **runs tonight over the 40 maps you already have** | new (offline tool); writes the existing `proposals/<fp>.json` shape |
| **2** | **Stage 5 review** | Needs 1 to have output; makes the whole thing usable end to end | reuses `ProposalSet::load`; new trust/source recording |
| 3 | Capture at sweep time | Small; makes every future map vision-ready at no extra load | `captureHostedEditor` exists |
| 4 | Stage 3 vision offline | Consumes 3's output; pure batch | new |
| 5 | Stage 0 worklist + marks filter | Turns it into a campaign rather than a row-picker | reuses `Marks`, scan result |
| 6 | Stage 1 controls-only terminal state | Unblocks fast sweeping; touches the gate, so not first | wizard change |
| 7 | Stage 4 screenshot inbox | Only unblocks the 19 bridged rows | new |
| 8 | Stage 0 timing + incremental cache | Real but smallest; resume already works | field, not mechanism |

**1 and 2 are the usable slice.** After them you can map with the existing wizard, run the
proposer, and review — the catalogue campaign works, just without vision or the worklist.
Everything after that reduces the review pile rather than enabling the flow.
