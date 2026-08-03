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
| 3 | **eq** | **3** | (a) generic band probing: primary group, map anchors, in-range ladder points; (b) fixture-as-test-case, re-prove 11/11 with identical numbers; (c) arm A scoped explicitly as fixture-only, plus the group-selection and Mono-Maker-role questions |
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

## 4e. Item 5a built: excitation plans serialised, and applied (3 August 2026)

Schema **2.2 → 2.3**. A map may carry `excitation`: an array of
`{index, value, semantic, why}`. Absent means unavailable, so every map
already written resolves to its suite's declared plan and stays probeable.

**`applyExcitation()` lands with the serialised form, and the inline writes are
deleted.** comp's excitation is now applied only through the plan. An unlanded
write or an out-of-range index is reported rather than assumed: the suite
stops with `INCONCLUSIVE (excitation did not apply)` rather than measuring a
compressor that was never put into the state its verdicts assume.

Proven end to end with a map declaring `ratio → 6:1` where the suite's own
plan says 10:1. The map's plan wins, the run measures **5.95 dB against 12.50
predicted**, and the suite-plan run still measures **6.41 against 13.50** —
the pre-parameterisation baseline, unchanged.

### What building it found: a declared plan can be silently overridden

The first working version resolved and applied the map's plan, printed
`excitation from map: ratio[2] -> 6.00`, and then measured at 10:1 — because a
later line re-established the excitation with `swRa.a.getLast()[0]`, the
ladder's top. **One inline write deleted, three left standing.** The report
asserted a state the measurement did not run in, which is the false-comment
class in output rather than in a comment.

Three fixes, all of the same shape — ask the plan, never the ladder:

1. the two re-establishing writes now use `compExc.valueFor("ratio", ...)`;
2. the **prediction** uses the ratio actually established, not the ladder's
   maximum — otherwise it predicts 10:1 behaviour from a plugin sitting at
   6:1 and reads the difference as the plugin's fault;
3. the evidence sentence prints the established ratio, having said
   `at ratio 10.0:1 predicts 12.50` about a number computed from 6:1.

The general lesson for the remaining suites: **deleting the first inline write
is not the same as deleting the path.** A declared plan is only the real path
when nothing else can establish the same state.

### Schema bump forced a better gate rule

Pinning `map schema == kMapSchemaString` in the corpus check meant every bump
broke the gate on real maps, leaving only two ways to green: rewrite the
corpus — mutating evidence — or never bump. The check now expresses the real
rule: this binary can **read** any map at or below its own schema, and maps it
**emits** carry the current string, pinned separately.

Drift gate 161 → 165, including three `applyExcitation` cases: every step
applied in order, unlanded writes reported rather than assumed, and an
out-of-range index refused rather than clamped.

---

## 4f. Item 5b built: control roles and enable links (3 August 2026)

Schema 2.3b. A named control may declare `role` and `enabled_by`:

```json
"Mono Maker": { "index": 7, "role": "stereo_width",
                "enabled_by": { "index": 8, "value": 1.0, "name": "Mono Maker In",
                                "why": "Mono Maker is inert until its In switch is engaged" } }
```

Role vocabulary, deliberately small because a role is a claim a suite can act
on rather than a description: `stereo_width`, `enable`, `bypass`, `mode`.

`controlWithRole()` refuses on none and on more than one, naming both
claimants — the rule `primaryGroup()` already applies to bands. An absent
`enabled_by` resolves to **UNKNOWN, never "nothing needs setting"**: a suite
that cannot tell those apart writes a parameter behind a disabled section and
measures its own silence.

**Enable links resolve into `ExcitationStep`s.** "Set index 8 to 1.0 so index 7
is live" is the same sentence as "set ratio to max so the compressor
compresses", so links go through `applyExcitation()` and an unlanded enable
write is reported by machinery that already exists. Drift gate 165 → 172.

### The standing question for an item that adds no verdict

5b introduces no measurement. The question becomes: **what will eq be able to
get wrong once the role and the link come from a map instead of from AMEK's
constants?** Today `controls["Mono Maker"]` at index 7 with index 8 as its gate
are facts about one plugin, verified once by a human and frozen in code. After
item 3 they are claims in every map, re-verified by nobody.

| # | What is wrong | What it produces | Caught? |
|---|---|---|---|
| a | Role names a control with **no** stereo effect | Arm A writes it, side energy does not move, **A5 fails** | The failure is caught; **the attribution is not**. Today an A5 failure means the harness is broken. With a map-supplied role the identical symptom means the map's claim is false. Item 3 must say which |
| b | Role names a **different** control that genuinely moves stereo width | A5 passes, A1 passes, arm A reports `contradicts` — **the right verdict through a mislabelled control** | **No, and nothing can, by behaviour.** Two controls with the same measurable character are indistinguishable by that character. The gate stays green while the map is wrong |
| c | Enable link has the wrong index, wrong value, or is absent | The gated control never becomes live, so writing it does nothing and **A5 fails — identical to (a)**, and identical to a dead index and to a bridged-write failure. Three causes, one symptom | Failure caught, cause not separated. Now separable: links go through `applyExcitation()`, which reports landing, so "the enable did not land" and "the enable landed and the control is still inert" are different facts |
| d | Enable link points at a **global** bypass or mode rather than a per-control enable | Engaging it changes the plugin generally, so **arm B** — the correct-map arm, which is not testing the link at all — measures a different plugin | **No.** Nothing checks that engaging an enable left the primary measurement where it was |

There is a fifth, quieter one: eq's **B5 negative control** asserts that Mono
Maker does not drift across arm B's writes. If the role names a different
control, B5 tests drift on the wrong parameter and passes for the wrong
reason — the same shape as (b).

### What item 3 must therefore build, decided before it starts

1. **Verify a claimed role by signal before acting on it**, with its own
   attribution: write the control, require the side band to move AND the mid
   band not to. That separates a width control from a gain or a filter, and it
   converts (a) and (c) from "the gate failed" into "the map's `stereo_width`
   claim is not supported by measurement".
2. **State the limit of that verification plainly**: it cannot separate the
   claimed width control from any other width control, so (b) is not
   falsifiable by behaviour. The map's identity claim for an
   equivalent-behaviour control is checkable only against itself, by the
   index/name cross-check item 2 built. Record it as a known limit rather than
   implying the role is proven.
3. **Null-test the enable** (d): measure the primary feature before and after
   engaging the link and require it not to move beyond σ_f, or record the
   movement as contamination. An enable that changes the measurement is not an
   enable.
4. **Distinguish the three causes behind one symptom** (c): unlanded write
   (reported by `applyExcitation`), landed-but-inert (role claim false), and
   index-out-of-range (refused at lookup).

None of this is speculative — every entry is a path that exists in arm A today
and is currently held correct by a constant.

---

## 4g. Item 3 (eq): the three-session split, proposed before starting

**A correction first.** The gate has **11 assertions, not 12**: B1–B6, A1, A2, A5, A4, restore. `A3` is an exclusions block that enumerates engaged modes and issues no assertion of its own. Both `M9_PAUSED.md` and this document said 12/12 — documentation asserting a property the code lacks, in our own records, including a line I wrote. The number to re-prove is 11.

### Session 1 — diagnostics and the role/enable machinery

Requirement 4 goes **first**, not last. It is diagnostic rather than functional, which is exactly why it gets dropped under pressure, and everything after it consumes its answer.

- **Cause triage** for the single symptom "the control did nothing": index out of range (refused at lookup, exists), write did not land (`applyExcitation` reports it, exists), write landed and the control is inert (**new** — requires a measurement). Output names the cause, never the symptom.
- **Role verification by signal** (req 1): a control claiming `stereo_width` is written across its ladder and must move the side band ≥ 4σ_side while leaving the mid band within σ_depth. Attribution is its own: *the map's `stereo_width` claim is not supported by measurement* — not *the gate failed*.
- **Enable null-test** (req 3): engage the link, measure the primary feature before and after, require no movement beyond σ_f. An enable that changes the measurement is not an enable.
- **The known limit** (req 2) printed wherever a role is used: this verification separates a width control from a gain or a filter, and **cannot** separate it from another width control. The role is supported, never proven.
- A5 and B5 both consume the verified role. B5's exposure is the same as A5's: under a map-supplied role its negative control would test drift on the wrong parameter and pass for the wrong reason.

**Provable at the end:** four specimens — a role naming a control with no stereo effect, an enable link at the wrong index, an enable link whose write does not land, and an enable link with global side effects — each producing a distinct and correctly attributed message; and AMEK still reporting 11/11 with identical numbers.

### Session 2 — per-semantic attribution and generic band probing. **This is where it gets hard.**

Not an even third. The difficulty is physical, not organisational:

`freq_hz`, `gain_db` and `q` are read from **one pair of Welch spectra** through `lobeFeatures`, which returns centre, depth and width **together**. Pooling is not a reporting choice here — it is how the measurement is taken. Worse, the three are **coupled**: a wrong q shifts the measured centre and depth; a wrong gain changes depth and, through lobe fitting, the apparent width. So one report per semantic requires **isolation probes** — move exactly one semantic at a time from a common reference state and attribute each feature to the parameter that actually moved. That is a restructure of arm B, not a refactor of its output.

Also landing here: `groups[].primary` instead of `groups[0]`; ladder points from the map with the chooser only when nothing pins them; and the conversion of B1/B3/B4 from `assertHarness` to `emitVerdict`, which closes M9's open item 1 for eq and makes the routing fork decide eq's language **for the first time** — its behaviour on octave-domain features is currently unproven.

**The tension to name now:** behaviour preservation says the numbers must not move, and isolation probes legitimately change the reference state they are measured from. If the numbers move, the honest outcome is a recorded explanation per change, not a forced match. A session that must produce identical numbers *and* a different measurement structure can only succeed by pretending one of the two did not happen.

**Planned relief valve, decided now rather than discovered at the end:** if session 2 overruns, isolation probes and per-semantic attribution land, and the `emitVerdict` conversion moves to session 3. Splitting there keeps a working gate at every boundary.

**Provable at the end:** three isolation specimens — corrupting only q, only `gain_db`, only `freq_hz` — each producing a contradiction naming exactly that semantic with the other two clean; each verdict carrying its own unit (octaves, dB, oct-log2 width ratio) with no "dB" on an octave measurement; and either identical numbers to the pinned run or a documented cause for each change.

### Session 3 — fixture-as-test-case, arm A scoped, full re-proof

- The eq fixture carries its ladder points (100 → 400 Hz, 0/+3/+6/+9 dB, q 0.71 → 2.00), the role and enable link for the AMEK case, and **arm A's injection**: which index to mis-map to and what it must do.
- One code path, two modes: assert against the fixture, or report without one.
- **Arm A refuses to run without a fixture-declared injection.** That is the fixture-only scoping made executable instead of documented — a deliberate mis-map needs a nominated wrong-index control of a specific character, which no general map declares.
- All 11 assertions re-proven with identical numbers; full regression across five suites; drift gate.

**The corpus question, needing a decision and not taken unilaterally:** the real AMEK map carries no `role` or `enabled_by`. Three options — (i) the fixture carries them for the regression case and the corpus gets them when AMEK is re-mapped under 2.3; (ii) AMEK is re-mapped now; (iii) tooling writes them into the existing map with provenance. **Recommend (i).** Option (iii) mutates a human-verified artefact to make our tests convenient, which is the pressure the schema-gate change was accepted to remove.

**Provable at the end:** `--gate-m9 eq` reproduces 11/11 with identical numbers; `--probe-batch` drives the same code path in report mode; arm A with no fixture refuses and says why.

### What item 3 does not buy

eq will read its band, roles and links from a map, but the only EQ map in the corpus is AMEK. "Runs generically" will be proven on constructed specimens, not measured on a second real EQ — the no-real-map limit, restated for eq specifically.

---

## 4h. Item 3, session 1 built: diagnostics, role verification, enable null-test

Requirement 4 first, as scheduled. `EjmapTriage.h` holds pure decision logic — no audio types — so the drift gate compiles it without linking the message loop and **every state it can report is provable in a test**, including the one that cannot be forced on hardware.

`idxMono = 7, idxMonoIn = 8` are deleted. The role and link resolve from the map, falling back to the eq fixture (signed option (i)) with the **claim from the fixture and the ladder always from the map** — a fixture carrying its own anchors would be probing itself. The fixture's index is cross-checked against the map's, and a disagreement stops the run.

### Cause triage: one symptom, three causes, three sentences

| State | The sentence says it is a statement about |
|---|---|
| `indexOutOfRange` | the **MAP** |
| `writeDidNotLand` | the **WRITE PATH** (bridge, message thread) — and nothing was measured, because nothing was set |
| `landedButInert` | the **PLUGIN**, or what the map claims this control is |

Only the third is about the plugin, and M9 has reported all three with the same words. Proven at unit level for all four states (the unlanded case cannot be forced on real hardware — writes land) and end to end on AMEK for the other three.

### The four specimens, each distinctly attributed

| Specimen | Result |
|---|---|
| `stereo_width` on **Input Gain** (a level control) | side 29.994 dB, mid 29.994 dB → *not a minority of the side movement* → **STOPPED** |
| enable link with the **value reversed** (`Mono Maker In → 0`) | triage: *landed and inert*; role: *does nothing measurable here* → **STOPPED** |
| enable link at an **unrelated index** (`V-Gain`) | **PASSED** — see the limit below |
| enable link pointing at the **global Power switch** | null-test: moved the primary feature **4.391 dB** → *not a per-control enable; every arm after it measures a different plugin* → **STOPPED** |

The null-test's stop takes precedence over the role check, which is the right order: a contaminated measurement invalidates the role result rather than the other way round.

### Two findings

**1. My first role criterion failed the signed fixture.** Requiring mid movement below 4σ_depth rejected AMEK: engaging Mono Maker moves the mid band 0.437 dB against a 0.352 floor. That number is neither a defect nor news — **arm A already records it, with its cause**, as *"recorded, not a criterion: M=(L+R)/2, so mono-ing below the crossover necessarily moves side content into mid. Expected physics."*

A new check must not silently assign a threshold to a quantity an existing check deliberately left unthresholded **with a documented physical cause**. Doing so re-litigates a settled question, and the first thing it disqualifies is the fixture the settlement came from. The criterion is now **dominance**: mid must be a minority (≤ 0.25×, the project's declared constant) of the side movement, which is what actually separates a width control from a gain. The absolute mid figure is reported the same way arm A reports it.

**2. A wrong enable link is invisible when the control is already live.** The `V-Gain` specimen passed, correctly: AMEK's Mono Maker is engaged by default, so a link pointing anywhere harmless makes no difference and nothing can tell. **An enable link is only falsifiable when it matters** — when the control would otherwise be inert. Discovered by attempting it rather than assumed, and it bounds what the null-test is worth: it catches links that do damage, not links that do nothing.

### Numbers, and what "identical" can mean

11/11 PASS. B3 (0.131) and B4 (0.026) reproduce exactly. **B1 does not, and did not before this change either**: four runs gave 0.0280, 0.0282, 0.0283, 0.0284 oct. The measurement is not deterministic across runs at the fourth decimal.

That matters for session 2's signed criterion. "Identical numbers where the measurement is unchanged" must mean **within the measured run-to-run spread**, and the spread is now measured for B1 rather than assumed to be zero. A criterion demanding bit-equality of B1 would be unmeetable by an unchanged build.

Drift gate 172 → 185. limiter, gate and saturation PASS; comp unchanged at 1 FAILED (the API-2500 over-claim).

---

## 4i. Item 3, session 2: per-semantic attribution. **Relief valve invoked.**

### What landed

**A declared reference state.** Arm B already moved one parameter per block, but the reference each block started from was implicit and different — B1 ran at whatever q the excitation left, B3 re-established freq but not q, B4 set gain but not freq. Nothing stated the reference and nothing verified it, so a reordering would have re-coupled the blocks silently. `bandRef` (100 Hz, +6 dB, q 1.0) is now established explicitly before each block.

**Isolation of the inputs is necessary and insufficient** — the point that makes eq different from comp. The features remain coupled no matter what order the parameters move in, so attribution rests on **per-semantic claim checks**: each of `freq_hz`, `gain_db` and `q` is written to the map's norm and its display read back, in **its own unit**, against **its own tolerance** (5% of the asked frequency, 1 dB, 0.25×q). Only when all three inputs are proven to be where the map claims is a feature failure attributable to its own parameter.

**The coupling, measured, from data the run already had.** B4 moves only q; the centre estimate moves with it:

```
COUPLING (recorded, not a criterion): moving ONLY q from 0.71 to 2.00 moved the
CENTRE estimate 94.0 -> 95.1 Hz = 0.0168 oct, against B1's centre tolerance of
0.0838 oct ... at 20% of B1's tolerance, a q defect can surface as a freq_hz failure.
```

That is eq's pooling shown to be physical rather than argued: a modest q change eats a fifth of the frequency tolerance, and a wrong q ladder eats all of it.

**"The other two held" was a sentence, not a check.** It is now `B-hold`, asserted after each block: the two parameters not being probed must still be at the reference (drift < 0.005). Three new assertions, all reading 0.00000.

### The three isolation specimens

| Specimen | Result |
|---|---|
| `q` ladder scaled ×0.25 (map says 1.0, plugin lands 4.0) | `CONTRADICTS q (3.000)` — freq_hz and gain_db both 0.00 |
| `gain_db` ladder scaled ×0.5 (map says +6, plugin lands +12) | `CONTRADICTS gain_db (6.00 dB)` — freq_hz and q both 0.00 |
| `freq_hz` ladder scaled ×0.5 (map says 100, plugin lands 200) | `CONTRADICTS freq_hz (100.00 Hz)` — gain_db and q both 0.00 |

Each names exactly the corrupted semantic, in that semantic's unit, with the other two clean. A q failure reporting as `freq_hz` — comp's defect in its worst form — is what these prove does not happen.

### Numbers, against the amended criterion

Assertions went **11 → 14** (the original 11 plus three `B-hold`). B3 (0.131) and B4 (0.026) reproduce exactly. B1 read 0.0282, inside the spread measured on the unchanged build before the work started (0.0280–0.0284). No forced match, no bit-equality demanded of a feature that was never deterministic.

A fixed explanatory clause was caught in the new code too: the contradiction message named q as the cause whatever had failed. Removed — the class is a week old and still reappears in the first draft of new text.

### Relief valve invoked: the `emitVerdict` conversion moves to session 3

Attribution landed; B1/B3/B4 still publish through `assertHarness`. The conversion is not mechanical — it hands eq's verdict language to the routing fork **for the first time on octave-domain features**, and the standing question applies to it: `routeVerdict` was derived and tuned on dB-domain magnitudes, so what it does with an octave feature against an octave tolerance is unmeasured. Answering that needs its own measurement, not the tail end of a session.

Session 3 therefore carries: the `emitVerdict` conversion **with its standing-question answer**, the fixture-as-test-case machinery, arm A scoped fixture-only, and the full re-proof — now of 14 assertions rather than 11.

---

## 4j. Item 3, session 3: the conversion, the fixture, arm A scoped. Item 3 closed.

### The standing question, answered by measurement before the conversion landed

`routeVerdict` is **dimensionless**: deafness is `|moved| < 4×floor`, over-claim is `||moved| − |pred|| > tolerance`. Nothing in either comparison knows whether the numbers are decibels or octaves, so it works on octave features arithmetically — which is what makes the hazard subtle.

eq's assertions used only the **tolerance** (`|meas − pred| ≤ tol`, a binary pass). Handing eq to the fork introduces a **second input those assertions never consulted: the floor** — and the floor is what decides between `contradicts` and `inconclusive`.

**So the newly falsifiable thing is a floor in the wrong unit.** Proven, not argued:

```
moved 0.20 oct, predicted 2.00 oct, tolerance 0.0838
  with the octave floor (0.0322)  -> 0.20 > 0.1288 -> over-claim  -> CONTRADICTS
  with the dB floor     (0.088)   -> 0.20 < 0.352  -> deafness    -> INCONCLUSIVE
```

Identical measurements, opposite verdicts, decided entirely by which floor was passed. 5a made the *unit* required at emit, but the floor was a bare `double` with no unit, so that guard did not cover this.

**The guard:** `Probe::Floor` pairs a value with its unit, and `emitVerdict` refuses when `floor.unit != unit` — a loud `HARNESS DEFECT` and no verdict, instead of a silent flip. Attempted via `--gate-m9 floorunit`, which emits the same measurement twice:

```
correct floor (octaves):        CONTRADICTS freq_hz (band centre): OVER-CLAIM ...
floor from the dB domain:       HARNESS DEFECT freq_hz (band centre): the verdict is in
                                'oct' and its floor is in 'dB' ... No verdict is issued.
```

The control case still produces a real contradicts, so the guard is not simply refusing everything. All five pre-existing call sites were forced to declare their floor's unit by the compiler: dB, dB, dB, `dB/dB`, `log2 ratio`.

The fork moved to `EjmapProbeRoute.h` for the same reason `EjmapTriage.h` exists — the drift gate compiles it without the message loop, so what it decides is provable in a test rather than only on hardware.

### The conversion: three units on one band

`freq_hz (band centre)` in **oct**, `gain_db (lobe depth)` in **dB**, `q (lobe width ratio)` in **oct-log2** — each with a floor in its own unit. M9's open item 1 is closed for eq.

### Fixture-as-test-case, and arm A refusing

The fixture now carries the reference state, the ladder points (100/400 Hz, 3/9 dB, q 0.71/2.0), the role, the enable link and **arm A's injection**. One code path, two modes, announced at the top of every run:

- `MODE: assert against the fixture — ladder points pinned, numbers comparable run to run`
- `MODE: report — the chooser picks them and the numbers are NOT comparable against a pinned run`

**Arm A refuses, and the refusal was attempted** (the fixture path is env-overridable precisely so it could be): with no declared injection, and with an injection nominating a control that is not the resolved one. Both refuse with distinct reasons and the gate reports `ARM B ONLY`. A refusal that has never fired is the misplaced-guard class.

### Re-proof: 14 results

11 assertions (three `B-hold`, B2, B5, B6, A1, A2, A5, A4, restore) plus 3 verdicts. The count holds; three of the fourteen changed **kind**, from harness assertion to parameter verdict, which is the conversion.

Against the amended criterion:

| Result | Status |
|---|---|
| `gain_db` 0.131, `q` 0.026 | **exact**, every run |
| `freq_hz` centre error | **0.0280–0.0286 oct** across 7 runs spanning the unchanged pre-session build and the converted one. Not deterministic, was never deterministic, and the conversion did not move it outside its own noise |

The spread was re-measured on this build before comparing, rather than reused from session 1's measurement — the earlier range was 0.0280–0.0284 and one run here read 0.0286, so quoting the old range would have understated it.

Regression: limiter, gate, saturation PASS; comp 1 FAILED (the API-2500 over-claim). Drift gate 185 → 189.

---

## 4k. Item 4 (saturation) and item 6 (the final comparison)

### The standing question for saturation

| Newly possible once drive index, ladder and excitation come from a map | Seen? |
|---|---|
| The map's **drive index** addresses another parameter | **Yes** — `crossCheckName`. Specimen: index moved to 39 → *"the map calls it 'Master Drive' and this instance calls it 'Mid Lo Drive Compensation'. A THD span measured through the wrong index would look like a perfectly ordinary saturation curve"* |
| The **excitation plan names a control that does not gate the drive** | **Yes**, after a fix — see below |
| An **offset drive ladder** (right span, wrong points) | **No, and correctly so.** Saturation makes no magnitude claim, so an offset ladder does not falsify anything it asserts. Stated rather than left as a gap: this suite cannot detect a stale ladder because it never predicts a magnitude from one |

**The asymmetry worth naming:** an *enable link* must be null — it makes a control live and changes nothing else. An *excitation step* must **not** be null — it exists to change the feature. They resolve into the same `ExcitationStep` type but carry opposite null requirements, and applying eq's enable null-test to saturation's XL stage would refuse a correct plan.

### The floor: unit-correct, quantity-wrong

Asked to check the unit rather than assume it, the answer is that the unit **was** right — and that is the finding. Saturation's floor was `InstrumentFloor::depthDb` = 0.088, eq's spectral **lobe-depth** floor, borrowed for a THD measurement. Both are decibels, so the floor-unit guard passed it.

**The guard protects the dimension, not the quantity.** A floor for a different feature in the same unit is invisible to it. Saturation now measures its own σ_THD by A/A pair (0.0000 dB on this deterministic subject) instead of borrowing eq's.

### The excitation plan, which was the whole blockage

The original diagnosis was "a bypassed stage". Measured: `[8] Master XL On` reads **On** and `[9] Master XL` reads **0 %**. The stage is enabled and its *amount* is zero, so Master Drive distorts nothing at any value. The plan declares both — the toggle as well, so the plan is the whole state rather than the part someone noticed.

Applied through `applyExcitation`, verified by signal per carve-out 2: **THD −145.41 → −45.00 dB, moved 100.41 dB**. The drive walk then produces real numbers for the first time: −45.00 → −9.70 dB, monotone.

Two defects fell out, both recorded as their own entries:

1. **A null hid a tautology.** The verdict passed `thdMoved` as measurement and `jmax(1.0, thdMoved)` as prediction — the same number — so it read `tracks` for any plugin. Invisible while THD never moved; the first excited run printed `moved 35.29 dB against 35.29 dB predicted`. Fixed by refusing the magnitude claim (drive is unitless; nothing predicts a THD magnitude from it) and deciding the falsifiable one: monotonicity.
2. **A noise floor cannot certify an effect.** Excitation was verified against 4σ_THD = 0.004 dB, so a plan naming Mono-Maker moved THD 0.15 dB and was reported VERIFIED — while the suite told the operator "the suppressor is not the plan". Now `max(1 dB, 4σ)`, matching comp and eq, and the message names the plan as the first suspect when it is unverified.

### Item 6: the final numbers, spread measured per feature first

| Fixture | Feature | Runs | Result |
|---|---|---|---|
| eq | `freq_hz` centre error | 7 | **0.0280 – 0.0286 oct — not deterministic** |
| eq | `gain_db` depth error | 7 | 0.131, exact |
| eq | `q` width ratio error | 7 | 0.026, exact |
| limiter | ceiling moved / worst | 3 | 22.50 vs 22.50, worst 0.02 — exact |
| gate | threshold moved / worst | 3 | 52.00 vs 54.00, worst 1.20 — exact |
| comp | threshold GR measured | 2 | 6.41 dB, exact |
| comp | ratio error | 2 | 0.020, exact |
| saturation | THD moved across ladder | 2 | 35.29 dB, exact |

**Exactly one feature in M9 is non-deterministic: eq's lobe centre.** Everything else reproduces exactly, run to run, across the whole parameterisation. Against the amended criterion that is the intended outcome — exact where the measurement is unchanged, within measured spread where it is not.

Suite status: eq `GATE M9: PASS` (14 results), limiter PASS, gate PASS, saturation PASS, comp 1 FAILED (the API-2500 over-claim, unchanged since before parameterisation). Drift gate 189.

### Carried gap, recorded so it is not lost

**comp has no `B-hold` equivalent.** Its three section headers — `THRESHOLD (excitation: ratio at max, verified)`, `RATIO (excitation: threshold at -30 dB, verified)`, `ENVELOPE (excitation: threshold -30, ratio max, verified)` — name states that nothing checks survived the writes between sections. Each section does write the state it names, so the claim is not false; "verified" refers to P4's one-time verification by signal, not to a re-measurement at that point. eq now asserts this and comp does not. Not fixed here, deliberately: eq's and saturation's sessions should not quietly edit another suite's assertions.

---

## Why resolution is INDEX-FIRST with the name as the check

Recorded because the intuitive order is the wrong one and someone will
otherwise "fix" it back.

A parameterised suite has two handles on a mapped parameter: the **index** the
map recorded, and the **name** the map recorded beside it. The instinct is to
resolve by name — names are meaningful, indices are opaque — and to treat the
index as a hint.

**The measurements on this project say the opposite.** Names survived every
version transition measured, including a `Bank` insertion that broke **339
indices** in one release. Indices are the volatile handle; names are the
stable one.

So the map is *authored* against indices because that is what a host writes
to, and the suite *resolves* by index because that is the only thing that can
actually drive a parameter — but the name is then used to **check** that the
index still points where the map thought. That ordering means a shifted index
is caught by a disagreement rather than silently obeyed:

```
CONTRADICTS threshold_db: the map's threshold_db points at index 1, which the
map calls 'Thresh' and this instance calls 'Attack'. A verdict computed from
this index would be about the wrong parameter, and every span it produced
would look plausible.
```

Resolving by name instead would quietly "repair" a stale map by finding the
right parameter under its new index — and the map would stay wrong, the tool
would report success, and the consumer that dials by index would still write
to the wrong place. **The probe must fail where the consumer would fail.**
Repair is a separate operation (M12's name-based transfer) and it must be a
decision, not a side effect of a verification pass.

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
