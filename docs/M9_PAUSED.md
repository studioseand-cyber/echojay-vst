# M9 paused pending the mapping campaign

**Date:** 2 August 2026
**HEAD at pause:** a3730f0, drift gate 130/130, tree clean
**Status:** paused 2 August 2026; **resumed and parameterised 3 August 2026** (items 0-6 of `M9_PARAMETERISATION_PROPOSAL.md`). This document describes the pause and the state at it; sections marked with a 3 August date describe the state after.

---

## Why we stopped

M9 was started immediately after the first successful ejmap tests (AMEK EQ 200 and oeksound spiff), on the reasoning that the audio probe is "what would catch a Mono Maker with no human present."

That reasoning is sound for an unattended pipeline. It is weaker for the actual deployment model: every map in the foreseeable campaign is made by Sean, or by a dedicated mapper working under supervision (TeamViewer or a sent build). A human is present at every map. The layers that serve that model are M4's corroboration-gated accept, M5's exclusion footer and the review screen's refusals, all of which are built and closed.

M9's own acceptance gate said the same thing when it ran. Of the three live defects that motivated the module:

- **case 1** (q reverting on a float ulp): a dial-path defect in the consumer that ejmap's tolerance rule cannot reach. Filed separately as `DEFECT_LADDER_TOLERANCE.md` on v2.
- **case 2** (1.2 kHz parsed as 1.2): caught in ejmap at map time.
- **case 3** (8 kHz clamped to a 780 Hz band, readback confirming the clamp): consumer-side. M9 supplies `measured_range` as evidence; the fix is elsewhere.

**One of three caught at map time.** The other two are consumer defects M9 arms rather than closes.

### The sequencing argument, which is the decisive one

**Maps made now are not invalidated by M9 arriving later.** The probe runs against a loaded instance and a finished map. It does not participate in building the map. Any map built during the campaign can be probed retroactively, in bulk, at any point.

The reverse dependency is real and currently unsatisfiable:

- Every threshold in M9 rests on **one subject per category** (eq: AMEK, compressor: API-2500, limiter: bx_limiter, gate: SSL X-Gate).
- The two declared constants (`0.25` and the `4σ` multiplier) carry a **20-map re-derivation plan** written into the proposal. There are two maps.
- The coverage claim (226 of 390 products) is stated against product counts, **never measured against real maps**.
- The API-2500 over-claim finding (a threshold ladder delivering ~45% of nameplate, confirmed by two independent estimators) is a **defect class with one member**. Whether it is endemic or an API-2500 quirk is unanswerable on this sample.

M9 needs a corpus. The corpus does not need M9. Finishing M9 first means finishing it blind and then discovering what was wrong when real maps arrive.

### The plain arithmetic

Nothing has shipped. Shipping EchoJay still dials the classifier maps that produced the original AMEK incident. Two human-verified maps exist against 390 dialable products. Every session spent on M9 is a session the real corpus does not exist.

---

## Pre-campaign check: cleared

Before pausing, one question was traced to its answer: does M3's sweep write through the same path that was defective in M9?

**No.** `sweepSetRead` (`tools/ejextract/EchoJayParamExtractor.h:243`) calls `param.setValue(n)` directly, then `cfg.settle()`, which `EjmapSweeper.h:144` installs as `runDispatchLoopUntil(50)`. The sweep has serviced the runloop after every write since M3, added for the bridged read-after-set (a `Thread::sleep` blocked the message loop the XPC reply needed, measured on API-2500). That reasoning happens to cover the SSL message-thread class exactly. The read is also a display read (`getCurrentValueAsText`), not a render, so the DSP-delivery failure mode does not arise on this path.

`writeConfirm` / `writeAndServiceRunloop` is M9's, and the probe is its only caller.

**Two latent notes, neither live:**

1. The settle hook is conditional (`EjmapSweeper.h:142-143`): installed only when called on the message thread. The off-thread fallback is `Thread::sleep(15)`, which does not service the runloop. Every current caller sweeps from the message thread. If anything later sweeps from a worker thread, this becomes the M9 defect one thread-context away.
2. `setValue` rather than `setValueNotifyingHost`. Different API from the one M9 measured. The display reads correctly through it and there is no evidence of a problem; noted as a difference, not a tested claim.

**The mapping campaign is not blocked.**

---

## Where M9 stands at the pause

| Category | Status |
|---|---|
| eq | confirms. Headline gate **11/11** (counted 3 Aug 2026; the "12/12" recorded earlier was stale — `A3` is an exclusions block that issues no assertion): a deliberate Mono Maker mis-map returns `contradicts` with zero human input and names the index from the side spectrum |
| compressor | ratio confirms (0.020 vs 0.100). threshold direction-provable, magnitude inconclusive at ~45% of nameplate, both estimators agreeing |
| limiter | ceiling confirms at 0.02 dB against a 1.88 tolerance |
| gate | threshold_db tracks: 52.00 dB measured vs 54.00 predicted, worst error 1.20 vs 4.50 tolerance |
| saturation | **verdict null RESOLVED 3 Aug 2026.** The stage toggle read On and its AMOUNT read 0 %, so drive distorted nothing at any value. With the excitation plan applied and verified by signal (THD -145.41 -> -45.00 dB), THD rises monotonically -45.00 -> -9.70 dB across the ladder. Direction confirms; magnitude is refused, because drive is unitless and nothing predicts a THD magnitude from it. Unblocking it exposed a tautological verdict that had been hidden by the null |
| de-esser | not built |
| delay | not built |

**Harness:** render harness, pump discipline, stakes and restore, sanity gate, σ_f extraction, excitation verification, the routing fork, the mode guard, the ambiguity rule, and the shared sensitivity check are all built and, where stated, proven by attempting the thing they refuse.

**Coverage, stated honestly** (rewritten 3 August 2026, after the parameterisation milestone).

The note this replaces said the suites *"cannot run on a second subject at all"* and that everything measured was *"a self-test of four fixtures"*. **That is no longer true.** All four suites now resolve their target, ladder and excitation from the map for the loaded fingerprint:

| Suite | Reads from the map | Verdicts, each in its own unit |
|---|---|---|
| eq | primary group's `freq_hz` / `gain_db` / `q`, the `stereo_width` role and its enable link | 3 — oct, dB, oct-log2 |
| compressor | `threshold_db`, `ratio` | 2 — dB, dB/dB (plus attack/release, inconclusive under undeclared units) |
| limiter | `ceiling_db` | 1 — dB |
| gate | `threshold_db` | 1 — dB |
| saturation | `drive`, and an excitation plan | 1 — direction only; magnitude refused, since drive is unitless |

**What that buys and what it does not.** The claim changed: a suite no longer asks whether a plugin agrees with its own display ladder, it asks whether the plugin does what **the map claims**. Those diverge exactly when a map is stale, made in another mode or preset, or made against another version — and every suite now stops rather than issuing a verdict when they do.

**THE STANDING CAVEAT: there is still no real map for any subject but AMEK and spiff.** Every map-driven result recorded in this milestone came from a **constructed specimen** — a self-map, or a self-map deliberately corrupted. The suites label it: a run with no map prints `SELF-CONSISTENCY CHECK, not a check of any map's claims`, and its verdict evidence carries `self-consistency only (no map)`. The machinery is proven; it has not yet been **used** on a map a human made.

**Coverage per category is unchanged: one to four parameters each.** Parameterisation changed *which subjects* a suite can run on, not *how much of a plugin* it decides. de-esser and delay remain unbuilt.

**The headline number.** On the one map any suite can run against today, AMEK EQ 200: **3 decided of 80 mappable slots**, and 0 of 39 on spiff, whose category has no suite.

---

## Feature 3's write-back: two decisions and their evidence

**`probed` is server-side only**, returned by `identities=`, never on the map
body. Nothing in the plugin consumes it and no case for it exists: refusing to
dial unprobed maps would refuse nearly everything, and dialing differently on a
probe verdict would act on a verdict a human already reviewed. Moving it onto
the map later is a smaller change than un-shipping bytes from every prefetch.

**`rev` is NOT bumped, decided from what `rev` is used for.** `stampRev`
(`lib/params-lib.js:325`) hashes `params / groups / controls / skips` only, and
the client compares `newRev` against `oldRev` in `storeParamMaps`
(`ChainHost.cpp:1500`) purely to detect content change. Probing changes none of
the hashed fields, so a bump would be a **false content-change signal**, costing
every client a refetch for a field it never sees. `stampRev` would not alter it
in any case.

**Neither write touches `plugin:<fp>:meta.ejmap.at`**, which is what the
supersession guard compares. A batch run restamping the corpus as freshly
human-submitted would be invisible until a second mapper existed, and would
then decide whose maps serve.

## CAMPAIGN PRECONDITION 2: preview and production share one KV instance

**Decided 3 Aug 2026.** All three Vercel environments — `production`,
`preview`, `development` — point at a single KV instance
(`docs/redis-region-move.md:61`). So a preview deployment does not write a
preview copy of anything; **it writes into the maps the shipping plugin
serves.**

**Content-addressing makes this deterministic rather than unlikely.** The fp
hashes `format|uid|version|paramCount`, so the same plugin build produces the
*same fingerprint* in both environments — a preview ingest lands on exactly
the production key, every time. A randomly-keyed store would collide by
accident; this one collides by design. The survey counts `plugin:*` at 4,434
keys and `pmap2:*` at 10,737 in that instance.

**The fix is Phase 0's instance split, NOT key scoping.** The runbook already
rejected scoping (`docs/redis-region-move.md:67`): *"Splitting the instance is
therefore the fix. The alternative, threading `scopedKey()` through every
unscoped call site … is strictly worse."* Splitting separates by connection
rather than by key, so it covers `plugin:*` and `index:*` for free. Scoping the
params family alone would be the rejected alternative in one corner, would be
unwound when the split lands, and would mean a **dual-read window across a
serving path** while a shipping plugin still reads the old shape.

**Deadline: before campaign volume, not before the backfill.** The backfill
adds a derived index over data that is already shared, so it changes no
exposure — and at two maps it is rebuildable in seconds. What changes the
exposure is a corpus of 1,376 maps, which is the real deadline.

## CAMPAIGN PRECONDITION: the supersession gap must close before anyone else gets a build

**Recorded here so it cannot be lost between now and then.** Two rules already
govern which map wins, and together they are correct for a single submitter:

- **ingest** (`lib/params-lib.js:632`): *"A newer human submission replaces an
  older one; a stale replay does not. A human map always replaces a crowd map."*
- **serve** (`api/params/maps.js`): *"a human-verified map is never
  second-guessed by ANY serve-time substitution."*

**The gap:** both key on `origin === "human"` and **neither distinguishes which
human**. A mapper's submission and Sean's are the same origin, so a mapper's
later map silently replaces Sean's on the same fp — and the serve-side guard
then protects *their* map from substitution. `tester_id` is stored in
`plugin:<fp>:meta` and consulted by nothing.

**Deadline, stated precisely:** it does not block Sean's own mapping, because
last-human-wins is correct with one submitter. **It bites the moment a second
person submits.** Features 1 and 2 make it visible; they do not make it safe.
**It must close before any build goes to another person.**

The shape of the fix is a rule about people rather than data: either ingest
weights owner over mapper, or a mapper's overwrite of an owner map lands in a
review state rather than serving.

## CAMPAIGN PRECONDITION: "submitted by you" rests on a local map, not on the server

**Same root as the supersession gap above, and the same fix closes both.**
`tester_id` is stored in `plugin:<fp>:meta.ejmap` and consulted by nothing.
`api/params/maps.js` returns the map BODY, and the map carries
`origin: "human"` but no `provenance` — so the response says *a human mapped
this* and cannot say *which human*.

Feature 2's column therefore attributes from local evidence: a map on this
machine for that identity means this machine did the work. Measured live on
3 Aug 2026 — AMEK and spiff both render ✓ "submitted by you" on that basis,
correctly.

**It is honest with one submitter and wrong the moment there are two:**

- a mapper's map on the server with **no local copy** renders as **unmapped**,
  so the campaign re-maps work that is already done;
- and if this machine happens to hold a local map for the same identity, the
  same row renders as **mine** when it is not.

The client cannot fix this from what it is given. The fix is server-side:
**return `tester_id` in the maps response**, alongside the supersession rule,
since both are blocked on the same field being served and both bite at the
same moment — when a second person submits.

**Deadline:** same as the supersession gap. It does not block Sean's own
mapping. It must close before any build goes to another person.

## TRANSPORT GAP: the redirect refusal has never fired live (3 August 2026)

Option A is built and wired to the submit card. Two of its three refusals are
proven against the real endpoint: a **401** with the token unset (HTTP 401 in
245 ms, queued `refused`) and a **timeout** (refused with cause, naming that the
request may or may not have arrived).

**The redirect refusal is proven only on `classifyReply`** — the function the
send path itself calls, so the running logic is the proven logic, but no 3xx has
ever crossed the wire. A genuine 307 (`POST /forgot`, confirmed by curl) returns
**no bytes** through the connection and the cause was not found.

**Consequence:** a real redirect today exhausts the deadline and reports as a
**timeout**. It fails safe in the right direction with the wrong reason —
both outcomes refuse, both queue as `refused`, neither follows the redirect nor
reports success, so nothing reaches an address the artefact does not name. But
the operator is told "may or may not have arrived" when the truth is "the server
answered and redirected", and those demand different responses: check before
retrying, versus the endpoint moved.

Recorded at `EjmapSend.h` as well, beside the function, because that is where
someone debugging a mystery timeout will be looking.

## Open items at the pause

Carried in the proposal; none blocks mapping.

1. **Emit path not narrowed.** Only 3 of 19 verdict sites route through `routeVerdict`. Compressor's two headline verdicts (the 45% over-claim and the ratio confirm) have never passed through carve-out 1. Unrouted is not incorrect, and neither is a null so the fork would likely send them where they already went, but that is an argument rather than a run.
2. **Excitation-plan requirement.** A suite can currently run without declaring one. Saturation did, and drove a bypassed stage.
3. **Saturation's excitation plan** (enable the XL stages, verify by signal per carve-out 2) and its re-run.
4. **de-esser and delay**, unbuilt.
5. **Waves census re-run** under the write fix. The measured 0.8% dead-index rate is an **upper bound** (a render-blind write reads as deaf, so the error is one-directional and the true rate can only be lower). That number decided the conflict card is an edge case rather than the primary interface; the decision is marked not-safe until re-run.
6. **`resolveSubjectByName`'s harness-miss branch** — built, correct, never fired. No specimen constructed.
7. **Loud termination gap.** The stake attributes death by name and stage; a silent exit-0 in a plugin's load path has nothing to time out. Catching it needs child-exit detection in the supervisor, out of M9's scope. Three MCompressor observations attached.
8. **Non-deterministic loads.** MCompressor and kHs Gate, both dying and recovering with no change to the load path, MCompressor producing three behaviours from one binary. No candidate explanation stands (the instantiating-walk theory was refuted by source inspection). Probe-quarantine release currently assumes deterministic failure. A third member means revisiting the rule.
9. **M/S and dual-mono products** trip the ambiguity rule on nearly every parameter. Correct behaviour, and it means those products need a qualifier source before any suite can decide them. M/S variants are common in saturation and compressor, so this is a real coverage question against the 226.

---

## What the campaign should feed back

When M9 resumes, these become answerable rather than argued:

- **Re-derive the declared constants** from the first 20 live maps: `0.25` (three distinct roles across seven uses: magnitude confirm on five features, time-constant ratio, lobe-existence floor) and the `4σ` multiplier. The 0.070-octave frequency gate is the plan's own signed 5% and does not re-derive.
- **Is the over-claim class endemic?** Walk each mapped compressor's threshold ladder against GR at fixed levels. One member today.
- **Real per-category coverage**, measured against mapped products rather than the 226 estimate.
- **How often carve-out 1 fires in practice**, which decides whether the conflict card is an edge case or the primary interface. Currently resting on a census marked not-safe.
- **A second member of the SSL message-thread delivery class** beyond X-Gate and X-Limit, and whether the class is vendor-framework-shaped as the evidence suggests (both members SSL; all three `processBlock`-class subjects different vendors).
- **Whether empty unit families are common.** Two specimens so far (API-2500 Attack, X-Gate Lower Threshold), on precisely the parameters where magnitude verdicts matter.

---

## Named defect classes recorded during M9

These outlived the module and belong to the project. Each is in `docs/M9_PROPOSAL.md` with instances.

- **A MEASUREMENT TAKEN AND NOT COMPARED IS NOT A CHECK.** (4 Aug 2026, named by the
  user.) The sibling of the rule below, and a nastier shape: not a missing measurement, a
  measurement *taken and discarded*. `describeFromRegistry` fetched the found component's
  real `AudioComponentDescription` into `actual`, on the line after the lookup, and never
  compared it to the description it had asked for. So when a zero-subtype wildcard returned
  a DIFFERENT product, the function was holding the evidence of its own error and returned
  the wrong name, vendor and uid anyway. Seventeen Soundtoys AUs collapsed onto
  Crystallizer's identity and a real EchoBoy map was filed under it. The tell is a local
  that is written and never read; the fix is to compare it and refuse, which is two lines
  and was always available.
- **VERIFICATION MUST READ WHAT THE CONSUMER READS.** Confirming a write by the writer's return value tests the writer, not the artefact. Five instances across four materials: `writeConfirm` reading the property plane that answers instantly; a stake id checked by eye rather than against `ScannedPlugin::pluginId`; three document writes checked only by the edit returning (shell-eaten backticks, a stale anchor, an anchor that never existed). Corollary: **a check that cannot fail is not a check.**
- **THE MISPLACED GUARD.** The guard is present, so reading the code shows it there; only running the path shows it absent. Eight instances. Two distinct fixes: move the guard to the choke point every path already goes through, or, where a path never reaches one, give that path its own recording. Naming the class did not prevent it — the newest suite reproduced it with the rule in front of it.
- **A NAME IS NOT EVIDENCE OF BEHAVIOUR.** Mono Maker mapped as `freq_hz`; X-Gate's Lower Threshold taken for its open point; bx_limiter's Release taken as a live probe parameter. Each time the name was correct and the behaviour was not what the name implied.
- **A NAME IS NOT EVIDENCE OF IDENTITY.** `bx_saturator V2` resolved to the UAD product, not the Plugin Alliance one. A subject resolved by name must have its identity printed before it is loaded, and a suite naming a specific product should match on the id.
- **CHECK A DEFECT IN THE OTHER SUITES BEFORE CONCLUDING THEY ARE UNAFFECTED.** The gate carried the identical Δ_pred defect and was invisible because a null feature absorbed the symptom. A suite already failing for another reason cannot report a second defect.
- **READ THE SOURCE BEFORE MEASURING THROUGH IT.** Five sessions of mechanism hypotheses about `runDispatchLoopUntil`, each fitted to cells produced by a call that did nothing. Fifteen lines of `juce_MessageManager_mac.mm` ended it. The source was available throughout.

---

## Resuming

Nothing needs re-deriving on resume. The proposal, the suites and the harness are committed at a3730f0 and the drift gate is green. Pick up at the open items above, in the order listed, with real maps behind them.
