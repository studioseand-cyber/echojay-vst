# M9 paused pending the mapping campaign

**Date:** 2 August 2026
**HEAD at pause:** a3730f0, drift gate 130/130, tree clean
**Status:** paused deliberately at a clean resting point, not abandoned, not blocked

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
| eq | confirms. Headline gate 12/12: a deliberate Mono Maker mis-map returns `contradicts` with zero human input and names the index from the side spectrum |
| compressor | ratio confirms (0.020 vs 0.100). threshold direction-provable, magnitude inconclusive at ~45% of nameplate, both estimators agreeing |
| limiter | ceiling confirms at 0.02 dB against a 1.88 tolerance |
| gate | threshold_db tracks: 52.00 dB measured vs 54.00 predicted, worst error 1.20 vs 4.50 tolerance |
| saturation | built, subject and preamble correct, verdict null pending an excitation plan |
| de-esser | not built |
| delay | not built |

**Harness:** render harness, pump discipline, stakes and restore, sanity gate, σ_f extraction, excitation verification, the routing fork, the mode guard, the ambiguity rule, and the shared sensitivity check are all built and, where stated, proven by attempting the thing they refuse.

**Coverage, stated honestly** (recorded in the proposal): M9 claims decisiveness on seven categories. The built suites decide **one to four parameters on one subject each** (eq 3, compressor 4, limiter 1, gate 1). A passing gate suite does not mean SSL X-Gate is cleared.

---

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

- **VERIFICATION MUST READ WHAT THE CONSUMER READS.** Confirming a write by the writer's return value tests the writer, not the artefact. Five instances across four materials: `writeConfirm` reading the property plane that answers instantly; a stake id checked by eye rather than against `ScannedPlugin::pluginId`; three document writes checked only by the edit returning (shell-eaten backticks, a stale anchor, an anchor that never existed). Corollary: **a check that cannot fail is not a check.**
- **THE MISPLACED GUARD.** The guard is present, so reading the code shows it there; only running the path shows it absent. Eight instances. Two distinct fixes: move the guard to the choke point every path already goes through, or, where a path never reaches one, give that path its own recording. Naming the class did not prevent it — the newest suite reproduced it with the rule in front of it.
- **A NAME IS NOT EVIDENCE OF BEHAVIOUR.** Mono Maker mapped as `freq_hz`; X-Gate's Lower Threshold taken for its open point; bx_limiter's Release taken as a live probe parameter. Each time the name was correct and the behaviour was not what the name implied.
- **A NAME IS NOT EVIDENCE OF IDENTITY.** `bx_saturator V2` resolved to the UAD product, not the Plugin Alliance one. A subject resolved by name must have its identity printed before it is loaded, and a suite naming a specific product should match on the id.
- **CHECK A DEFECT IN THE OTHER SUITES BEFORE CONCLUDING THEY ARE UNAFFECTED.** The gate carried the identical Δ_pred defect and was invisible because a null feature absorbed the symptom. A suite already failing for another reason cannot report a second defect.
- **READ THE SOURCE BEFORE MEASURING THROUGH IT.** Five sessions of mechanism hypotheses about `runDispatchLoopUntil`, each fitted to cells produced by a call that did nothing. Fifteen lines of `juce_MessageManager_mac.mm` ended it. The source was available throughout.

---

## Resuming

Nothing needs re-deriving on resume. The proposal, the suites and the harness are committed at a3730f0 and the drift gate is green. Pick up at the open items above, in the order listed, with real maps behind them.
