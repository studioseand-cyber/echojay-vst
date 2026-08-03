# Proposal: subject-parameterising the M9 suites

**Date:** 3 August 2026
**Status:** proposal, nothing built
**Cause:** the batch runner (`--probe-batch`, eaf6167) established that the suites cannot run on a second subject at all. Everything M9 has measured is a self-test of four fixtures. The pause doc's coverage note is amended to say so.

---

## 0. What this changes about M9, stated before the detail

Today the suites sweep the plugin live and compare a rendered feature against the ladder they just read from that same plugin. That is a **display-versus-render** test: it proves the plugin's own display agrees with its own DSP. It is a real test — it caught the API-2500 over-claim — but it is not the test M9 exists for.

A parameterised suite reads the ladder **from the map** and asks whether the plugin does what *the map claims*. Those differ whenever the map is stale, was made in another mode or preset, or was made against another version. The first is plugin self-consistency; the second is map correctness. **Parameterisation is not a refactor that preserves the claim — it upgrades the claim**, and that is the reason to do it rather than a side effect.

Both are worth keeping. Where a map's anchors and a fresh sweep disagree, that disagreement is itself a finding (stale map, or mode-dependent ladder), and it is invisible today because only one of the two is ever consulted.

---

## 1. What welds each suite to its subject

Classified per weld: **F** = fixture fact (true only of this product), **C** = category fact (true of the category, safe to keep), **F/C** = a category fact currently expressed as a fixture constant.

### eq — the most welded, and the only suite that already reads a map

| Weld | Where | Class |
|---|---|---|
| Subject id `aumf,ameq,Brwx` | `MainComponent.h:1660` | **F** |
| `groups[0]` — probes the first group | `:3329` | **F/C** — `groups[].primary` exists in the map and is the right selector |
| freq/gain/q indices **and anchors** read from the map | `:3331-3337` | **C** — already correct, and the model for the others |
| `idxMono = 7, idxMonoIn = 8` hardcoded | `:3334` | **F** |
| `controls["Mono Maker"]` by exact name | `:3338` | **F** |
| Ladder points 100 Hz → 400 Hz | `:3407`, B1 | **F/C** — "two octaves apart, inside the band's range" is the category fact |
| Gains 0 / +3 / +6 / +9 dB; q 0.71 → 2.00 | B3, B4 | **F/C** — same shape |
| Excitation = +6 dB on the band's gain | `:3412` | **C** |
| Decorrelated mid/side stimulus | `:3351` | **C** |
| **Arm A: deliberate mis-map to Mono Maker** | `:3334`, arm A | **F** — needs a control that is not a filter *and* moves stereo width. No map declares such a thing |
| Printed identity "AMEK EQ 200", "LF Gain 1" | `:3340`, `:3416` | cosmetic **F** |

Arm A is the headline gate's whole point and the most fixture-bound thing in M9. It cannot generalise to an arbitrary EQ without a map that nominates a wrong-index control of a specific character.

### compressor — welded by parameter *names*, not indices; reads no map at all

| Weld | Where | Class |
|---|---|---|
| Subject id `aufx,APCM,ksWV` | `:2858` | **F** |
| `idxOf("Thresh")`, `"Ratio"`, `"Attack"`, `"Release"`, `"Output"` — **exact** name equality | `:2872` | **F** — "Thresh" is a Waves spelling |
| `idxOf("Knee")`, optional | `:2974` | **F/C** |
| Ladders from live sweeps, not from a map | `:2878-2893` | **C** today, **the thing to replace** — the comment says outright: "no compressor map exists yet, so the suite builds its ladder the way the tool would" |
| Excitation: ratio→max, threshold→−30 dB | `:2934-2935` | **C**, assuming a dB-domain threshold ladder that spans −30 |
| Probe levels −30/−20/−10; threshold points −20/−15/−10/−5 | `:3028-3029` | **F/C** — fixed absolutes that assume ladder coverage |

### limiter / gate — least welded; two name assumptions and a subject, nothing else

| Weld | Where | Class |
|---|---|---|
| Subject **by name** `"bx_limiter True Peak"` / `"SSL X-Gate"` | `:1985-1993` | **F**, and by the *wrong mechanism* — saturation was pinned by id after `bx_saturator V2` resolved to the UAD product; this suite substring-matched names. Same class. **FIXED 3 Aug 2026**: pinned to `aumf,bxtp,Brwx` (limiter) and `aufx,XGAT,SSLN` (gate), identity printed before load, both suites re-run and still PASS |
| Target by preference list `"Ceiling"`/`"Output Ceiling"`, `"Upper Threshold"`/`"Threshold"` | `:2043-2044` | **F/C** |
| Comment claims the target comes "from the map when one exists" | `:2040-2041` | **no map is read anywhere in this branch** — the comment described behaviour that does not exist. **FIXED 3 Aug 2026**, plus the two console lines carrying the same implication; recorded in `M9_PROPOSAL.md` as its own mechanism |
| Ambiguity refusal, demonstrated on the bare pattern first | `:2029-2038` | **C** — exemplary, keep as is |
| Ladder from a live sweep of the target | `:2134-2142` | **C** → replace with map anchors |
| Burst gap = 3× *this plugin's own* max release, read from its ladder | `:2060-2081` | **C** — the model for how a suite should adapt |
| Render-plane check moves `"Release"` found by name | `:2088-2132` | **F/C** — needs *some* known-live parameter; picking Release by name is the fixture part |
| 4 ladder fractions 0.15/0.4/0.65/0.9; expressible-range clipping | `:2175`, `:2257-2296` | **C** |

### saturation — pinned correctly, but has no verdict yet

| Weld | Where | Class |
|---|---|---|
| Subject id `aufx,bxa2,Brwx`, identity printed before load | `:2467-2471` | **F**, by the right mechanism |
| Drive **pinned by index 34** with a name guard | `:2499-2519` | **F**, explicitly acknowledged in the comment |
| Harmonics at 997 Hz × h | `:2488` | **C** |
| No excitation plan; verdict null | open item 3 | blocking |

---

## 2. What a parameterised suite needs from a map

### The bridge already exists

`EjmapAssignment.h:52-83` (`DialSets::forCategory`) is the category→semantic vocabulary the suites should key on instead of names or indices:

- compressor → `threshold_db`, `ratio`, `attack_ms`, `release_ms`, `makeup_db`, `knee_db`, `mix_pct`, `input_db`, `output_db`
- limiter → `threshold_db`, `ceiling_db`, `release_ms`, …
- gate → `threshold_db`, `attack_ms`, `release_ms`, `ratio`, `output_db`
- eq → `freq_hz`, `gain_db`, `q`, `low_cut_freq_hz`, `high_cut_freq_hz`, `output_db`
- saturation → `drive`, `mix_pct`, `input_db`, `output_db`

Each mapped record carries what a suite needs to drive it: `index`, `name`, `kind`, `unit`, `range`, `anchors`. Groups carry `n`, `primary`, `params{}`, `freq_range`.

### Per suite: what it reads instead

| Suite | Reads from the map | Replaces |
|---|---|---|
| eq | the group where `primary == true`; its `freq_hz`/`gain_db`/`q` index + anchors; band's `freq_range` to choose two in-range points ~2 octaves apart | `groups[0]`, 100/400 Hz, 0/+3/+6/+9, q 0.71/2.00 |
| comp | `threshold_db`, `ratio`, `attack_ms`, `release_ms`, `knee_db` indices + anchors; probe levels from the threshold ladder's own span | `idxOf("Thresh")` etc., live sweeps, fixed −30/−20/−10 |
| limiter | `ceiling_db` index + anchors | `"Ceiling"`/`"Output Ceiling"` name list, live sweep |
| gate | `threshold_db` index + anchors; `release_ms` for the burst gap | `"Upper Threshold"` name list, live sweep of Release |
| saturation | `drive` index + anchors | pinned index 34 |

### What no map currently carries

1. **An excitation plan.** Which parameters must be set, and to what, before the effect is measurable at all. comp hard-codes ratio→max/threshold→−30; saturation drove a *bypassed* stage because nothing told it to enable one. This is the single biggest gap, it is already open item 2/3 in the pause doc, and every suite needs it. It is per-product, not per-category.
2. **Semantic roles for non-dial controls.** `Mono Maker` is in `controls` with index, kind, unit and anchors — but nothing says *stereo width*. Arm A, M/S disambiguation and any negative control need the role, not the name.
3. **Companion / enable links.** Index 8 gates index 7 on AMEK (`Mono Maker In`). Band on/off and section bypass are the same shape. A suite that writes a parameter behind a disabled section measures nothing and cannot tell.
4. **A nominated negative control** — a parameter that must *not* move the feature. Mono Maker plays this role on AMEK by hand.
5. **The plugin's channel mode.** `mode` in the map is the *session's sweep mode* (`"fast"`), not M/S versus stereo. Open item 9 says M/S products trip the ambiguity rule on nearly every parameter; nothing in the map resolves it.
6. **Which member of a pair is meant** — resolved *if* the mapper assigned the semantic to the right index, which is exactly what the probe is supposed to check. Circular for the target parameter, fine for supporting ones.

Items 1–3 are schema work (`kMapSchemaVersion` is 22; this is a 2.3 conversation). Items 4–6 can ride on them.

---

## 3. Where the signed fixtures go

They are the only regression evidence M9 has, so the rule is: **parameterising must not delete a single assertion, and must be provable by re-running them.**

**Proposal: a fixture becomes a test case, not the suite's only mode.**

A fixture file per signed subject (`tests/fixtures/m9-eq-amek.json`) carries what is fixture-specific today: subject id, map fp, the ladder points to use, the expected verdicts, the tolerances, and — for eq — the arm A injection (which index to mis-map to, and what it should do). The suite becomes generic and takes a subject + map. Then:

- `--gate-m9 eq` = the generic suite, run against the fixture case, **asserting** the recorded expectations. All 12 assertions survive verbatim. This is the regression gate and it stays in the pre-commit path.
- `--probe-batch` = the same generic suite, run against any map, **reporting** without expectations.

One code path, two modes: assert versus report. The distinction is the fixture file's presence, not a second implementation — a second implementation is how the two drift apart.

**Arm A stays fixture-only, deliberately.** It needs a nominated wrong-index control of a particular character, which the fixture file can carry and an arbitrary map cannot. Scoping it as fixture-only is honest; pretending a generic map can produce it is not.

**The proof the refactor is behaviour-preserving:** re-run all four fixtures after each suite is converted and require the same verdicts *and* the same numbers within the measured σ_f for that suite. Not "the gate still passes" — the same numbers. A refactor that changes a measurement while keeping a pass is the failure mode this is guarding against.

---

## 4. The honest size

Not uniform. eq is roughly four times gate.

| # | Piece | Sessions | Why |
|---|---|---|---|
| 0 | **Shared machinery**: fixture-file format, assert-vs-report modes, map-reading helpers (`semanticIndex`, `anchorsFor`, `primaryGroup`), and the ladder-point chooser that picks in-range points from a map's own anchors | **1–2** | All four suites need it. Building it inside the first suite and extracting it later is the misplaced-guard shape |
| 1 | **gate + limiter** | **1** | Least welded, and they share one body. Subject from the map, target from `threshold_db`/`ceiling_db`, ladder from map anchors. Also fixes the by-name subject resolution to by-id — the same defect saturation already had fixed |
| 2 | **compressor** | **1–2** | Five name lookups → five semantics; probe levels derived from the threshold ladder instead of fixed absolutes. Two sessions if the derived levels change the measured numbers, which must then be explained rather than accepted |
| 3 | **eq** | **3** | (a) generic band probing: primary group, map anchors, in-range ladder points; (b) fixture-as-test-case, re-prove 12/12 with identical numbers; (c) arm A scoped explicitly as fixture-only, plus the group-selection and Mono-Maker-role questions |
| 4 | **saturation** | **1**, blocked | Parameterisation is small (one pinned index → `drive`). It is blocked behind its excitation plan, which is its own session and is not this work |
| 5 | **Schema 2.3**: excitation plan, control roles, enable links | **2** | Section 2's gaps 1–3. Can run in parallel with 1–2, but eq (3) wants control roles, so it lands before eq |
| 6 | **Final: all four fixtures, numbers compared** | **1** | The behaviour-preservation proof |

**Total: 10–12 sessions.** Order: **0 → 1 → 2 → 5 → 3 → 4 → 6.**

Reasoning for that order: prove the shared machinery on the cheapest suite (gate/limiter) before committing it; do comp next because it is the one with a live scientific question (the over-claim class) that a corpus will need; land the schema before eq because eq's remaining welds are precisely the things the schema is missing; eq late because its regression evidence is the one that must not break; saturation last because it is blocked on work that is not this.

**What this does not buy.** After all of it, coverage is still one to four parameters per category — but on *any* subject in that category rather than one. The corpus question ("is the over-claim class endemic?") becomes answerable; the coverage question does not improve until suites decide more parameters, which is separate work.

---

## 4a. Schema-ordering check (asked before starting, 3 August 2026)

**The question:** do items 0, 1, 2 and 5 commit to a schema shape before 2.3 is designed?

**Answer: yes, but the exposure is concentrated in exactly one concept, and it is not the one the ordering suggested.**

Going through the 2.3 contents separately rather than as a block:

| 2.3 concept | Who needs it | Exposure in 0/1/2 |
|---|---|---|
| **Excitation plans** | comp (0/1/2 era), saturation, eventually all | **Real.** comp's excitation (ratio→max, threshold→−30) *is* an excitation plan written inline. Item 0's fixture files would carry one too. Three local encodings of one concept, invented before the concept is designed |
| **Semantic roles for non-dial controls** | eq only (arm A, M/S) | None — item 3 is already after item 5 |
| **Enable / companion links** | eq only (index 8 gates 7) | None — same |

So the only genuine collision is the **excitation plan**, and it appears in item 2, not item 5.

**The map-reading helpers are safe.** `semanticIndex`, `anchorsFor`, `primaryGroup` and the ladder-point chooser read `params[sem].index`, `groups[].primary` and `anchors[]` — all schema 2.2, all present in the two live maps, and `chainSlotsXml`-style frozen. 2.3 is additive.

**The fixture-file format is low-risk but not zero.** Fixture files are test artefacts versioned with the tests, not corpus data; there are four of them and rewriting one is cheap. They may also legitimately stay more specific than any map, since a test case is allowed to pin what a general map cannot.

### Two candidate reorderings, and why not the obvious one

**Option A — design excitation plans first (2.3a → 0 → 1 → 2 …).** Rejected. It designs the serialised shape with *zero* parameterised consumers, which is precisely how M9 got here: four suites finished blind against two maps, then the batch runner discovered what was wrong the moment real maps arrived. Designing a schema for excitation before any suite consumes one repeats that exactly.

**Option B — one interface early, serialisation after two real consumers.** Recommended. Item 0 gains a required deliverable: **`ExcitationPlan` as a named type with a single resolution point**, not inline setup code in each suite. Items 1 and 2 consume it — item 1 mostly with an empty plan, item 2 with the real one comp already has. Then 2.3a designs the *serialised* form informed by two working consumers, and only the plan's **source** changes (in-code → map), behind an interface that does not move. That is the mouth's one-builder-two-callers lesson applied before the duplication rather than after it.

**Does item 1 need excitation at all?** Checked rather than assumed: no. The limiter reads its expressible range from the signal, and the gate reports `threshold UNDEFINED, not fitted` when no burst is attenuated. Missing excitation degrades those suites to an honest inconclusive, never to a false verdict. comp is the first real consumer.

**Migration.** 2.3 fields are additive and optional; existing maps stay valid, and a suite treats an absent excitation plan as *no plan* — absent-key-means-unavailable, the convention already used for metering. `kMapSchemaVersion` 22 → 23 and the pinned drift-gate strings move together when 2.3a lands.

### The order the check produces

**0 → 1 → 2 → 5a → 5b → 3 → 4 → 6**

- **0** shared machinery, **now including the `ExcitationPlan` interface**
- **1** gate + limiter (least welded; proves the machinery; empty plans)
- **2** compressor (first real excitation consumer)
- **5a** schema 2.3a: excitation plans serialised, designed against two consumers
- **5b** schema 2.3b: control roles + enable links
- **3** eq (needs 5b), **4** saturation (blocked on its own excitation plan), **6** final numbers comparison

Net change from the accepted proposal: item 5 splits in two, 5a moves one slot earlier, and item 0 grows one deliverable. The original order was substantially right; the risk it carried was one concept, and the fix is an interface rather than a reordering.

---

## 4b. Item 0 built, and one finding that changes item 3 (3 August 2026)

`EjmapSubject.h` ships the map-side lookups: `primaryGroup`, `slotFor`, `slotInGroup`, `controlNamed`, `spreadAcrossLadder`, `octavesApartWithin`, and `resolveExcitation` as the single excitation resolution point with the 2.3a hook already branched. Drift gate 136 → 161 checks. Every refusal is tested by attempting it, because refusing where a fixture constant used to assume is the whole feature.

Against the real corpus the new path resolves the **same indices** as the fixture route (`groups[0]` → 29 / 24 / 34), which is the behaviour-preservation proof in miniature.

**The finding.** Asked for a centred two-octave pair inside AMEK's primary band, the generic chooser returns **54.1 → 216.3 Hz**. The fixture uses **100 → 400 Hz**. Both are two octaves, both sit inside the band's 15–780 Hz range, and they are different measurements.

So a parameterised eq suite driven by the generic chooser **cannot reproduce the fixture's numbers**, and the behaviour-preservation rule proposed in section 3 — same verdicts *and* same numbers within σ_f — would be unsatisfiable as written.

**Consequence, which resolves rather than weakens the design:** the fixture file **must carry its ladder points**, and the generic chooser is what runs when no fixture pins them. That is exactly what "a fixture becomes a test case" has to mean — the fixture's job is to hold the specific numbers that make a regression reproducible, and a test case that lets a chooser move its own stimulus is not a regression test. Item 3 adopts this: fixture pins points, generic path chooses them, and the assert mode compares against the pinned run.

Discovered by running the chooser against a real map rather than by reasoning about it, which is the only reason it surfaced before item 3 rather than during it.

---

## 4c. Item 1 built: gate and limiter read the map (3 August 2026)

Both suites now resolve `ceiling_db` / `threshold_db` from the map for the loaded fingerprint, drive the **map's** norms, and measure the feature against the **map's** predicted landing. The live sweep is kept as the second opinion rather than the source of truth, and the disagreement between them is reported.

**Behaviour preserved against pinned points.** Fixtures carry their ladder points (`tools/ejmap/tests/fixtures/`), the chooser runs only when nothing pins them. With no map on this machine both suites reproduce their pre-parameterisation numbers exactly: limiter moved 22.50 dB against 22.50 predicted, worst 0.02 dB vs tol 1.88; gate moved 52.00 against 54.00, worst 1.20 vs tol 4.50. eq is untouched and still reports `GATE M9: PASS`.

**The weaker claim is labelled.** With no map, the run prints `DISPLAY-VS-RENDER SELF-CONSISTENCY CHECK, not a check of any map's claims`, and the verdict evidence carries `self-consistency only (no map)`. Map-driven runs carry `MAP-DRIVEN (feature vs the map's claim)`.

### The divergence, with numbers

Against a **self-map** (the live ladder in map shape — circular as verification, kept as a specimen so the check is shown *not* firing as well as firing):

```
map says -25.50 at norm 0.0784 -> plugin displays -25.48   |diff| 0.02
map says -18.00 at norm 0.2258 -> plugin displays -17.98   |diff| 0.02
worst 0.02 over 4 points  <- the map's ladder and this plugin agree
```

Against a **stale map** (the same self-map with every value shifted 6 dB, as a map made in another mode or preset would be):

```
map says -25.50 at norm 0.1940 -> plugin displays -19.49   |diff| 6.01
map says -18.00 at norm 0.3681 -> plugin displays -11.98   |diff| 6.02
worst 6.02 over 4 points  <- THE MAP AND THE PLUGIN DISAGREE
```

This is the upgrade made visible for the first time. A ladder read live from the plugin cannot express this failure at all: the suite would have driven the plugin's own numbers and confirmed them against themselves.

### What building it found: PARAMETERISATION CREATED A CLAIM THE FORK CANNOT EXPRESS

The first stale-map run returned **`CONFIRMS`**, carrying `worst |feature - ladder| 7.20 dB vs tol 1.88` inside the evidence string of a passing verdict.

`routeVerdict` decides whether a feature **moved** as predicted — a span claim. A uniformly offset ladder moves by exactly the right amount with every point in the wrong place: on the 6 dB specimen the span moved 19.46 dB against 19.50 predicted, so the span check alone says *tracks*. Per-point accuracy never entered the routing; it only rode in the evidence.

**This failure was unreachable before item 1.** While the ladder came from the plugin itself, an offset between map and plugin could not exist by construction. Parameterisation introduced the claim, and shipping item 1 without noticing would have produced a suite that confirms stale maps — strictly worse than the self-consistency check it replaced.

**Fix:** a fourth emit function, `emitContradicts`, mirroring the existing split and structurally unable to express a confirm; and per-point accuracy now gates the span verdict whenever the map drives the ladder. The stale specimen now returns `CONTRADICTS ... the feature tracks over its SPAN but each point lands away from where the map says`, and the suite reports FAILED.

### The excitation rule, tested by attempting it

Neither suite declares an excitation plan, and the claim was that a missing plan degrades them to an honest inconclusive rather than a false verdict. Attempted with a map pinning the ceiling at the top, so the limiter can never engage:

```
INCONCLUSIVE ceiling_db: gain reduction stayed below 1 dB at every one of the 4
ladder points, so the feature never existed to measure. Candidate causes, in
order: the plugin was never engaged (no excitation plan is declared for this
suite -- none), the stimulus does not reach the ladder's range, or the target
index is not the ceiling.
LIMITER SUITE: INCONCLUSIVE (feature never appeared; not a verdict)
```

The guard runs before any routing, so the route narrative never gets to describe a feature that did not exist. Two smaller corrections came out of it: `emitInconclusive`'s fixed suffix asserted *no measurement was taken*, which is false at this site — four renders were taken and the feature never appeared — so the basis is now stated per call; and a first attempt on the gate produced an honest inconclusive by a *different* path (span did not move → carve-out 1), which is why the guard needed its own specimen rather than being credited with that result.

---

## 4d. Item 2 built: comp reads the map (3 August 2026)

### The standing question, answered before converting

*What can now be wrong that could not be before, and does the verdict machinery see it?*

Before conversion, comp found indices by exact name on the live instance and swept its ladders live. So the index always addressed the parameter whose name matched, and the ladder always described the plugin in front of it. Four things become possible the moment the map supplies them:

| # | Newly possible | Seen by the machinery as it stood? |
|---|---|---|
| 1 | The map's **index** addresses a different parameter (a Bank insertion broke 339 indices on this project) | **No.** Every span it produced would look plausible |
| 2 | An **offset threshold ladder**: right GR span, every point wrong | **No.** The threshold verdict routes on `totalGr`, a span — item 1's class exactly |
| 3 | A **ratio ladder with correct values and wrong norms**: the plugin lands elsewhere on a curve the map describes correctly | **Partly.** The slope-delta shifts, but only a large error exceeds tolerance |
| 4 | A **declared but wrong unit family** (ms against a seconds ladder) scaling every time prediction by 1000 | **No.** The suite refuses *undeclared* units; declared-and-wrong was the gap |

Answered with machinery, not argument: an index **range guard** and a **name cross-check** for 1, and the shared **map-claim check** with a **per-point gate** for 2, 3 and 4. Resolution is by index because that is what the map carries; the name is the check on it, deliberately the reverse order — names survived every version transition this project measured, indices did not.

### The specimens, all four run

**Control — self-map** (the live ladders in map shape). Worst 0.00 dB over 4 threshold points and 0.00:1 over 2 ratio points, and the verdicts are **identical to the pre-parameterisation baseline**: threshold OVER-CLAIM 6.41 dB measured against 13.50 predicted, error 7.09 vs tol 3.38; ratio CONFIRMS 0.38 against 0.40, error 0.020 vs tol 0.100. That is behaviour preservation for comp, against pinned points.

**Candidate 1 — index shifted to Attack, still named `Thresh` in the map:**
```
CONTRADICTS threshold_db: the map's threshold_db points at index 1, which the map calls
'Thresh' and this instance calls 'Attack'. A verdict computed from this index would be
about the wrong parameter, and every span it produced would look plausible.
```

**Candidate 2 — threshold ladder offset 6 dB, span intact:** threshold worst **6.03 dB**, ratio 0.00 — contradicted, correctly attributed.

**Candidate 3 — ratio values right, norms wrong:** threshold 0.00, ratio worst **4.00:1** (`map says 2.00 → plugin displays 6.00`) — contradicted, correctly attributed. The 10:1 point displays `Inf`, a real landing rather than a parse failure, and is reported as unreadable rather than counted.

### What building it found

**Pooled evidence names the wrong parameter.** The first implementation kept one claim report for the whole suite, so candidate 3 — a ratio failure with every threshold point exact — was reported as `CONTRADICTS threshold_db`. Routing right with wrong words is the failure the verdict fork exists to prevent, and it reappears wherever evidence from two parameters is merged before it is attributed. Now one report per semantic, each gating its own verdict, in its own units (dB for threshold, `:1` for ratio).

**A hand-built ladder is the wrong specimen, twice.** Constructed "agreeing" maps failed at 4.52 dB (comp) and 7.20 dB (limiter) because both assumed a ladder linear in norm; API-2500's threshold is 10 → −0.48 → −20 across norms 0 → 0.5 → 1. The self-map dump exists for exactly this, and is now one shared builder rather than a copy per suite.

**The inconclusive basis, twice wrong.** A fixed suffix asserted *no measurement was taken*, false at the excitation guard. Replacing it with a bool produced a second fixed suffix asserting *the feature never appeared*, false at the envelope sites where tau moved 11.5 → 63.7 ms and the obstacle was an undeclared unit family. The basis is now a free string supplied by the caller, because only the caller knows why no verdict is reachable. Two rounds of the same named class in three lines of code.

### ExcitationPlan: the type earns itself, and shows one gap

comp declares `ratio → max` and `threshold → −30` through the named type, with a reason per step (`a compressor at 1:1 compresses nothing`). It prints as one line and is now the first real consumer, so schema 2.3a will serialise a shape that already has a working consumer.

**What it is missing, found by using it:** the type carries steps but nothing *applies* them — comp still writes its own excitation inline, and the declaration is descriptive. A plan that is declared but not applied can drift from the code that does the work, which is the false-comment class waiting to happen. 2.3a should land `applyExcitation()` beside the serialised form so declaration and application are the same act.

---

## Appendix: the uid-ambiguity refusal, tested (3 August 2026)

The other three runner refusals were proven by attempting them. This one is now too, and it is load-bearing rather than defensive.

`EchoJayAuRegistry.h:308-311` records that JUCE's uid (`type ^ subType ^ manufacturer`) is not unique: 2 collisions across 4 Waves components. Recomputed independently from the installed components' `Info.plist` files (1361 components, no plugin code run), the same two collisions appear:

```
uid 594f7d63   Waves: Sibilance-Live (m)  aufx SILM ksWV
               Waves: EMO-Generator (s)   aumf SIGS ksWV
uid 594f7d7d   Waves: Sibilance-Live (s)  aufx SILS ksWV
               Waves: EMO-Generator (m)   aumf SIGM ksWV
```

A map carrying uid `594f7d63` and category `eq` was put in front of `--probe-batch`:

```
SUBJECT Sibilance-Live (m) (map identity AudioUnit|594f7d63|14.0.0) -> 2 candidate(s)
subject unresolved          1
    Sibilance-Live (m) -- uid 594f7d63 matches 2 installed components (the uid XOR is
    not unique): Sibilance-Live (m) AudioUnit:Effects/aufx,SILM,ksWV | also
    EMO-Generator (s) AudioUnit:Effects/aumf,SIGS,ksWV
```

0 probed, 0 posted, both candidates named. Without the refusal the runner would have taken the first match — a de-esser — and probed it as an EQ.
