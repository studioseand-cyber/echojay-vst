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
| Subject **by name** `"bx_limiter True Peak"` / `"SSL X-Gate"` | `:1985-1993` | **F**, and by the *wrong mechanism* — saturation was pinned by id after `bx_saturator V2` resolved to the UAD product; this suite still substring-matches names. Same class, unfixed here |
| Target by preference list `"Ceiling"`/`"Output Ceiling"`, `"Upper Threshold"`/`"Threshold"` | `:2043-2044` | **F/C** |
| Comment claims the target comes "from the map when one exists" | `:2040-2041` | **no map is read anywhere in this branch** — the comment describes behaviour that does not exist |
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
