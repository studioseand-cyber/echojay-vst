# ejmap: Implementation Plan

**Date:** 29 Jul 2026
**Companion docs:** `EJMAP_BUILD_SPEC.md` (the what), `PARAM_MAPPING_HANDOFF.md` (the why)
**Status:** executable plan, module by module, with acceptance gates
**Target:** `echojay-vst-v200`, new CMake target `tools/ejmap`

---

## 0. What this plan changes from the spec

The spec is sound. Four additions and three decisions change it.

### 0.1 Decisions taken

| Question | Decision | Reasoning |
|---|---|---|
| Verification depth | Write-back, read-back, human visual, **audio probe**, **AI rehearsal**, **regression on version change** | Read-back verifies addressability only. The probe verifies meaning from the signal. This is the only layer that would have caught Mono Maker with no human present. |
| Pass depth | **Three modes** (Fast, Deep, Repair), not one | "Every parameter observed" and "90 seconds per plugin" cannot both be true on a 60 param plugin. Splitting them preserves both, and per key trust already exists to carry the difference honestly. |
| AI assist | Direct Anthropic API, local key, **behind a provider interface** | Phase 1 speed. The interface means the tester build swaps to the backend as a config change, not a rewrite. |

### 0.2 The three modes

Tier 2 named controls are nearly free. The tool sweeps every parameter regardless (it needs the noise mask and the flat detector across the whole set). Recording name, anchors, range and unit for every swept parameter costs one struct. The expensive operation is the human wiggle and visual confirm, which is per parameter and irreducible.

So depth is a property of **observation**, not of **coverage**.

**Fast mode** (default, testers; targets measured and re-based at the M4 gate record: proposal-backed lane 30 s, bare lane ~10 s/row)
- Every parameter swept, sanitized, recorded as a Tier 2 named control, `trust: setread`
- Dial set only is human observed and probed, `trust: human-verified`
- Groups pattern inferred and bulk confirmed
- Result: complete control surface, honest split trust

**Deep mode** (Sean, top ~50 plugins, no time limit)
- Every parameter human observed, wiggle captured, `ui_hint` recorded
- Every parameter probed where the category suite can reach it
- Tier 2 controls promoted to `human-verified`
- AI assist available on every row
- Result: the reference maps, and the labelled dataset for classifier training

**Repair mode**
- Open an existing map by fp or product, re-verify or re-map named keys only
- Used after a plugin update, after a disagreement flag, after a probe contradiction

Mode is recorded per key in the payload. A map is never "verified" as a whole.

### 0.3 The four additions, and what they actually buy (see 0.4 for what measurement later changed)

**Skip button.** Not a skip, three distinct recorded outcomes:
`not_present` (plugin has no such control), `not_automatable` (control exists on the GUI, no parameter moves), `deferred` (human unsure, comes back). Each persists with a reason string. A skipped key is absent from the map and present in the evidence. This codebase has six confirmed silent drops. A fourth-instance skip that vanishes would be the seventh.

**Approval sheet and in plugin labelling.** A host cannot know where a knob is on screen. It can know where the **mouse** was at the instant the poll loop detected the change. That single observation gives a screen coordinate per captured parameter, which produces:
- an overlay pinned onto the plugin's own editor showing the semantic name over the knob the human touched
- a labelled screenshot per plugin, saved as a QA artifact and shipped with the evidence
- a `ui_hint: {x, y, w, h}` in the payload, normalised to editor bounds
It is capturable only at that moment and costs one `Desktop::getMousePosition()` call.

**Screenshot to the API.** *(Superseded by measurement, see 0.4: `createComponentSnapshot` returns a BLANK image for every editor tried; the working source is window-level capture of our own window, nil when minimised.)* The editor is hosted inline. Two images per query: the full editor, and a tight crop centred on the `ui_hint` for the parameter in question. The vision call receives image, parameter table, sweep data, classifier verdict, and what has been mapped so far. That is the exact context needed to answer "which of AMEK's 15 frequency class parameters is band 1 frequency".

**Test after approval.** Two layers, section 6 and section 7.5.

### 0.4 Superseded by measurement (single reconciliation pass, 31 Jul 2026)

This plan was written before the tool existed. Building it replaced several of
its claims with measurements; each is corrected AT the section it lives in,
and this table is the index. A cold reader should trust the notes below over
any un-annotated sentence that contradicts them.

| Plan said | Measured | Detail at |
|---|---|---|
| `createComponentSnapshot` gives clean editor images, no permissions | **Blank for every editor tried.** Window-level capture of our own window works (even occluded / 60% offscreen), returns nil when minimised. Overlays draw BEHIND native editor views, so "pinned" labels cannot draw over plugin GUIs | §0.3, §7.3, M8 |
| 30 Hz poll, 3-second noise baseline | Detection is snapshot-relative so the rate only affects disambiguation and ui_hint; rate is **calibrated per plugin** from measured sweep cost (reads are 0.03–0.24 µs in-process, 50–80 µs bridged). Baseline demoted to a short probe: only **73 of 1,074,600** params are real exposed meters; retroactive promotion (with human-evidence suppression) is the primary defence | M2 |
| Channel twins from correlated multi-moves | Zero confirmed twins in 4,233 maps; the MC 77 "precedent" was an inference from a name list. `Kind::twins` retired to descriptive shape fields. The ONE lockstep pair ever observed live (AMEK's Param Link channel mirror) is resolved by a human pick, never asserted | M2 gate, M5 |
| M1 gate "roughly 1,536 entries / 1,267 distinct" | **1,419 AU components, 1,376 candidates** after the census filter; 622 AU bridged (47.7%), 335 of 861 VST3 x86-only (cannot load), 1 product arm64-only | M1 |
| Registry walk "reused verbatim" | Lifted into a SHARED header (`EchoJayAuRegistry.h`, seam 3.2) with a census-driven filter and byte-identical baseline proof — the same lift-and-pin method later used for the sweep header | M1 |
| 90 s EQ / 30 s compressor targets | Measured: the 30 s figure belongs to the proposal-backed lane (26% coverage). The bare lane — 74% of plugins — measures **~10 s/row**; target adjusts, not the measurement | §0.2, M4 gate record |
| One group, `"n": <band count>` | One group object **per band**, `n` = band number. The plan's own example was the error's source | M10 payload example |
| Schema 2.1 | **2.2 shipped**: ui_hint carries `editor_w/editor_h/screen`; `evidence.readback` entries carry `asked`; `captured_by` values are `poll / poll+gesture / gesture / human_pick`; skips have a fourth outcome `mode_material`; controls entries carry `kind`, `labels`, `identity_display`, display-declared `unit` | M10 |
| Display parsing is one problem | Three liar classes measured: flat (getText formats current state), **identity display** (hosting fabricates a 0–1 display; 9 whole-plugin products), and **getText lying about set positions** (bridged AU; caught by set-then-read spot checks). Plus the unit-switch collapse (`942.5 Hz` → `1.0 kHz`) handled by display-declared unit families | M3, M6 |
| Readback verifies the write | On **bridged AUs** the immediate readback reads the PRE-write display and falsely reverts correct writes — a live defect in shipping EchoJay, filed on v2 as `DEFECT_BRIDGED_READBACK.md` with census (604 of 622 bridged are Waves) | M6, v2 filing |

---

## 1. Placement, branch, build isolation

### 1.1 Repo and target

`echojay-vst-v200`, new target `tools/ejmap`, alongside `tools/ejextract`.

Same repo is non-negotiable per the spec: `ejmap` and EchoJay must compile the same `EchoJayParamApply.h`. A submodule or a vendored copy drifts, exactly as `usableCoreCount` drifted and read `bx_digital V3` as not dialable.

### 1.2 Branch

Branch from `feat/param-band-matcher`, not from `v2`.

That branch carries the band matcher, bidirectional interpolation, segment interpolation, and the read-back comparator. `ejmap` needs all four, and needs the same ones EchoJay will ship. Branch name: `feat/ejmap`.

Merge order later: `feat/param-band-matcher` reaches `v2` first, `feat/ejmap` rebases on the result. `ejmap` touches no file that Session B touches, so it does not participate in the `ChainHost.cpp` collision.

### 1.3 Build isolation, given the known coordination hazards

All four coordination hazards in the handoff bit at least once. Pre-empt them:

- **Dedicated worktree.** `~/Documents/ECHOJAY FILES/ECHOJAY VST/ejmap-wt`, one Claude Code session, never shared.
- **Dedicated build directory.** `build-ejmap/`, never `build/`.
- **No AU install path.** `ejmap` is a standalone app. It writes to `/Applications/ejmap.app` or runs from the build dir. It cannot overwrite a plugin build, and a plugin build cannot overwrite it.
- **Version on screen.** Same convention: the title bar shows the build version and the git short hash. The on screen version is the only proof of what is running.
- **Before any build:** `git status` clean check, and specifically confirm the 30 s TTL override in `ChainHost.cpp` is not present.

### 1.4 The shared code contract, enforced

Not a convention, a test.

**`ejmap-roundtrip-test`**, a separate small target:
1. Load a map produced by `ejmap`
2. Run it through EchoJay's `applySettings` from `EchoJayParamApply.h`
3. Assert the writes are byte identical to the writes `ejmap` verified during mapping
4. Run against a corpus of every map in `~/Library/ejmap/maps/`

Runs in the pre-commit hook on `feat/ejmap`. If the shared header changes and the two paths diverge, this fails before the commit lands.

Additionally: a `static_assert` on a `kMapSchemaVersion` constant defined once in the shared header, referenced by both binaries.

---

## 2. Architecture

```
                     ┌─────────────────────────────────────┐
                     │  ejmap (standalone JUCE app, macOS) │
                     └─────────────────────────────────────┘
                                     │
   ┌─────────────┬───────────────────┼───────────────────┬─────────────┐
   ▼             ▼                   ▼                   ▼             ▼
SCAN         HOST                CAPTURE             PROBE          ASSIST
registry     AudioPluginFormat    poll @30Hz         offline         Anthropic
walk         instance             + listener         render          vision +
(ejextract)  native editor        + mouse pos        + analysis      chat
   │             │                   │                   │             │
   └─────────────┴─────────┬─────────┴───────────────────┴─────────────┘
                           ▼
                    SESSION STATE
                    per plugin: params[], sweeps[], captures[],
                    assignments[], groups[], probes[], skips[]
                           │
                  ┌────────┴────────┐
                  ▼                 ▼
            SHARED HEADERS      APPROVAL SHEET
            EchoJayParamApply   every row, evidence,
            EchoJayParamExtract labelled screenshot
            schema + sanitizer  per row approve/reject
                  │                 │
                  └────────┬────────┘
                           ▼
                    PAYLOAD v2.1
                    local JSON → upload → structural gate → store
```

**Threading.** Message thread owns the editor and the UI. A dedicated capture thread owns the 30 Hz poll (parameter reads are thread safe on both formats). A worker thread owns sweeps and probes so the editor stays live and the human can watch the knob move. Audio render runs offline on the worker, not through a device.

---

## 3. Module specs

Each module: purpose, build items, acceptance gate, known failure modes. Build in order. A module is not done until its gate passes against **stored** data, never against the code's intent.

---

### M0: Skeleton and shared code contract

**Purpose.** Get the target compiling and the drift guard in place before any feature.

**Build**
- `tools/ejmap/CMakeLists.txt`, JUCE GUI app target, links `juce_audio_processors`, `juce_audio_utils`, `juce_dsp`
- Includes `Source/EchoJayParamApply.h` and `tools/ejextract/EchoJayParamExtractor.h` directly, no copies
- `kMapSchemaVersion` moved into the shared header, `static_assert` in both binaries
- `ejmap-roundtrip-test` target, empty corpus initially
- Pre-commit hook extension on `feat/ejmap`

**Gate.** `ejmap` builds and launches an empty window. `ejmap-roundtrip-test` compiles and passes on an empty corpus. Deliberately break one shared function signature and confirm both binaries fail to build.

---

### M1: Scan and host

**Purpose.** Enumerate installed plugins, instantiate one, show its editor inline.

**Build**
- ~~Reuse `--au-registry` walk from `ejextract` verbatim~~ **As built (seam 3.2):** the walk was LIFTED into a shared header, `tools/ejextract/EchoJayAuRegistry.h` (`buildCensus`, `describeFromRegistry`), with instruments and unused types counted and dropped via the census, never silently — and ejextract proven byte-identical against a captured baseline after the lift.
- VST3 file walk alongside it, same code path as `--bootstrap`
- Instantiate via `AudioPluginFormatManager`, 48 kHz, 512 block, stereo in/out
- Host the editor in a resizable child window, respect `isResizable`, handle editors that report zero size (some plugins size only after a paint cycle: retry after 200 ms, then fall back to a fixed 800x600 with a warning row)
- **Crash quarantine.** In process hosting means a plugin crash kills the app. *(As built, this grew beyond the sketch: a per-site watchdog with `_Exit(87)` and stack capture, frame-based crash attribution, an auto-relaunch supervisor with crash-loop guards, and quarantine release — manual for loads, automatic for scans when the bundle changes on disk.)* Write `~/Library/ejmap/inflight.json` before instantiation, clear it after successful editor open. On launch, if `inflight.json` exists, mark that plugin `crash_on_load`, add to quarantine, and continue. This is the ejextract worker isolation idea adapted to a tool that must show a GUI.
- Ledger with the same outcome vocabulary as ejextract: `ok`, `crash_on_load`, `timeout`, `license_refused`, `no_editor`, `no_params`

**Gate.** *(Numbers as measured, replacing the estimate:)* Enumerate this machine: **1,419 AU components, 1,376 candidates** after the census filter. 622 AU are bridged via AUHostingServiceXPC and load; 335 of 861 VST3 are x86-only and cannot load. Load and display editors for 20 plugins across formats and vendors, including one Waves shell component, one Plugin Alliance, one Valhalla. Kill the app mid load and confirm the quarantine marks it on relaunch.

**Failure modes.** Editors that open offscreen on a second display. Plugins that demand a license dialog on instantiate (already a known ejextract outcome, carry it through). Plugins whose editor requires a live audio callback to paint (start a silent playback graph on load).

---

### M2: Capture engine

**Purpose.** Turn a human touching a knob into a parameter index, with a screen coordinate.

**Build**

*Baseline and noise mask.* *(Superseded by measurement, see 0.4: real exposed meters are 73 in 1,074,600, so the 3-second baseline was demoted to a short probe with escalation-on-hit, and RETROACTIVE PROMOTION — counting only appearances with no human evidence behind them — is the primary defence. Promotions are recorded rows, visible and releasable.)* On plugin load, before any human interaction, probe briefly with nothing touched; anything self-changing joins `noise_mask[]`, excluded from capture and recorded in the evidence.

*Poll loop.* *(Superseded: the fixed 30 Hz is retired. Detection is snapshot-relative, so rate only affects sequential-edit disambiguation and ui_hint; it is calibrated per plugin — `clamp(0.30/sweep_seconds, 4, 30)` — because a parameter read costs 0.03–0.24 µs in-process and 50–80 µs bridged.)*
```
snapshot = [getValue(i) for i in 0..n]  // on arm
each tick:
  current = [getValue(i)]
  moved  = { i : |current[i] - snapshot[i]| > eps and i not in noise_mask }
  classify(moved)
```
`eps` is per parameter: for a discrete parameter with `num_steps`, half a step; for continuous, `1e-4`. A single global epsilon misses fine controls and false-fires on coarse ones.

*Classification of `moved`*
| Case | Action |
|---|---|
| empty for 8 s | prompt: "nothing moved, is this control automatable?" → `not_automatable` |
| exactly one | capture, record `ui_hint` from mouse position |
| two or more, all moving in the same direction with correlated magnitude | *(superseded: `twins` retired — see the gate note below; shape recorded as `same_direction`/`magnitude_ratio`, never asserted)* |
| two or more, uncorrelated | *(superseded: this is a `gesture` — a first-class outcome naming its members; the human picks, the rest stay `co_moved`)* |
| more than 8 | almost certainly a preset change or a modulation source → discard, warn |

*Listener layer.* Subscribe `AudioProcessorParameter::Listener` and `gestureChanged`. When gesture begin/end fires on exactly one index, that is authoritative and instant. When the listener is silent (a known class of plugins), the poll still works. Record which mechanism fired in `evidence.captured_by`.

> **Built in M2 pass three (2026-07-31)**, with one deliberate narrowing: gesture evidence *resolves* a multi-parameter move only when exactly one moved index gestured — zero decides nothing (the silent class), two or more is ambiguity again (a mirroring plugin may report both sides of a link), and both fall through to the human picker. The poll stays the detector; the listener is the disambiguator. Rows carry `captured_by`: `poll`, `poll+gesture`, `gesture`, or `human_pick`. Gesture reports also join the promotion-suppression probe, covering touches the mouse ring cannot see (MIDI controllers, and any input that never involves our mouse). Proven by self-test on Pro-Q 3 AU: two parameters moved in one window with a gesture on one → `captured` idx 18, `captured_by: gesture`, follower kept as `co_moved` — no human pick.

*Mouse capture.* At the tick that detects the change, `Desktop::getInstance().getMousePosition()`, converted to editor-local and normalised to editor bounds. Store with a small default bounding box (a 48x48 region around the point, tunable). If the mouse is outside the editor at capture time (host automation, MIDI learn, a keyboard entry), record `ui_hint: null` rather than a lie.

**Gate.** Turn a set of controls and confirm the captured index's `getName()` matches the control turned. **Passed as restated 2026-07-31: 12 of 12 distinct controls on Pro-Q 3 VST3.** The row originally demanded confirmation against "a documented parameter order"; no such docs are available from FabFilter, and they would not be the right check anyway — the tool reads `getName()` per index, so the name is the plugin's own claim about that index. What was verified is that the plugin's claim matched the control the human turned, 12 of 12. A stronger check (name vs an authority independent of the plugin) would need vendor docs that do not exist for most of the corpus. A multi-parameter gesture records every moved index, names them, and does not claim to know which control the human touched. On a plugin with a moving gain reduction meter, confirm the noise mask excludes it and capture still works.

> **Two gate rows retired 2026-07-30. Both were specified from parameter names and dissolved on measurement.**
>
> **The MC 77 twins row.** It read: "On MC 77, confirm the L/R attack pair is captured as `indices[]` and not as a single index (this is the exact bug the map format already fixed once)." The parenthetical was false — `indices` appears nowhere in the shipped apply path, no commit ever added it, and 0 of 4,233 extracted maps use it, so there was no format support and no fix. The claim was an inference from MC 77's parameter list (`Attack L`, `Attack R`, `Link`), not an observation. MC 77 is dual-mono: two channel strips, two knobs, one knob writes one parameter. Waves API-2500 and Q1 were checked next and behaved identically. **No confirmed case exists, so this row cannot be satisfied and no longer blocks M2.** `Kind::twins` is retired to a descriptive `same_direction` / `magnitude_ratio` pair on a `gesture` result; `indices[]` stays in the schema against a real case turning up on a tester's machine.
>
> A poll could not have satisfied the row anyway. It sees a delta vector, so one control writing two parameters, a link mirroring a value onto its partner, and two merely-correlated parameters are one event to it. Separating them needs a reliable touched-parameter signal, which is the listener layer above — and even that is not guaranteed, since a mirroring plugin may report gestures on both or neither.
>
> **The moving-meter row** narrowed the same way: only 73 of 1,074,600 extracted parameters have both a readout name and a flat sweep, so exposed-meter-as-parameter is rare rather than typical. The noise mask still earns its place, but the baseline was demoted to a short probe rather than a 3-second one.
>
> *Live confirmation (2026-08-01, AMEK re-submission run):* four VU Meter parameters became **the first real readout-shaped parameters found on this machine** — getText lied at spot check, setread refused (`flat via gettext`), and set-then-read readback-verify failed. The rarity measurement holds (4 in one plugin against 73 in the corpus), and the refusal machinery caught all four without a human having to notice anything: rare, not imaginary, and handled by behaviour rather than by name.

**Failure modes.** Trackpad micro movement causing a spurious `ui_hint` drift. Plugins that update parameters at block rate with tiny dither. Plugins whose GUI writes on mouse-up only (poll catches it, the 8 s timeout must not fire early).

---

### M3: Sweep, anchors, text liars

**Purpose.** Turn a captured index into a value curve.

**Build**
- Set-then-read sweep, 21 points, **shared code from `EchoJayParamExtractor.h`**, not reimplemented
- Flat detection: if two set points produce identical display text, the parameter is a text liar candidate. Confirm with a third point before flagging.
- **Sanitize on capture**, using the shared sanitizer: pair sort descending with a visible `anchorsReversed`, trailing junk truncation via longest monotonic run, bipolar mirror rejection, plateau rejection
- **Curve view.** Draw the sanitized curve, mark rejected points in a different colour, show what was truncated. The human can reject the whole set and force the typed path.
- **Typed anchor path** for text liars: set n = 0.0, 0.25, 0.5, 0.75, 1.0, prompt for the GUI reading at each, parse with the shared display parser, build anchors, mark `method: "human-typed"`. Add a "same as displayed" quick accept when the GUI value is legible in the screenshot (the AI assist can pre-fill these, see M7).
- Record `method` per parameter: `gettext`, `setread`, `human-typed`

**Gate.** Sweep a Valhalla reverb, confirm every parameter flags as a text liar, complete the typed path on one, and confirm the resulting anchors interpolate correctly through `EchoJayParamApply.h`. Sweep mpressor's threshold and confirm the descending anchors (16 dB at n=0, −18 dB at n=1) survive the sanitizer with `anchorsReversed` set, and that a request for −18 dB writes n=1 and not n=0.

**Failure modes.** The bare-k and `NkM` display parser cases are fixed but the fix must be the shared one. Parameters whose display changes format across the range (`900 Hz` then `1.2 kHz` then `12k`). Plugins that need a moment to settle before the display updates (add a settle delay, tunable, default 15 ms, raise to 50 ms for known slow vendors).

> **Added from measurement (2026-07-31):**
> - **There are two liar signatures, not one.** The flat sweep (getText formats current state — ValhallaVintageVerb VST3) reads as no information and earns the setread retry. The **identity display** (every value equals its own norm — ValhallaVintageVerb AU, where the hosting layer fabricates a normalized display when the AU provides no string-from-value) reads as a *plausible ascending curve* and defeats flat detection entirely. Flagged behaviourally as `identity_display` when ≥5 anchors all satisfy |v−n| < 0.005: a candidate marker, never a verdict, since a genuine unitless 0..1 control looks identical. Identity anchors dial correctly in norm terms; unit-bearing requests need the typed path.
>   **Scope, corpus-measured (2026-07-31):** raw prevalence is 58.9% of 1,074,600 parameters, but it is dominated by MIDI CC banks and preset slots that honestly display 0..1. Among control-shaped parameters: 3.2% (VST3) / 4.4% (AU), and the *whole-plugin fabricated* cohort — the Valhalla mechanism, ≥90% of control params identity — is **9 distinct products** (7 AU: ValhallaVintageVerb, ValhallaDelay, Omnisphere, CamelCrusher, TH-U Slate, Virtual Mix Rack, Playhead; plus Massive X and TR5 Suite on VST3). The typed path is therefore the **fallback**, not the main path, and pass two was scoped accordingly.
> - **The gate's mpressor subject is dead on this machine.** elysia mpressor AU crashes in its own render under the silent pump with no mutation in flight (3 of 3 loads, backtraced; quarantined with the evidence), and its VST3 is x86-only. The descending-anchors row was measured on **Waves API-2500 (m) Thresh** instead: 21 anchors, 10 → −20 dB, `anchors_reversed`, bridged AU, none rejected.
> - **Sweeps mutate what the pump renders.** setread and the state-restore bracket are not specified against a concurrent processBlock, so every sweep runs under `PluginHost::pausePumpForMutation()` (pause + drain). The extractor never met this hazard because it never ran audio.

---

### M4: Assignment UI

**Purpose.** The keyboard driven loop that produces semantics.

**Build**
- **Load the classifier's verdict as the proposal.** Source: `classified-v2/` locally if present, otherwise fetch the served map for that fp. Never present a blank form.
- Sort rows: `unsure` and low confidence to the top, then unmapped dial set entries, then confirmed
- Category picker, pre filled, with the dial sets from the spec (eq, comp, de-esser, delay, reverb, sat, plus gate, limiter, transient, channel_strip, amp_sim to match the store's categories)
- Per row state machine: `proposed → armed → captured → swept → probed → confirmed`
- **Keyboard map:** `SPACE` confirm, `N` not present, `A` not automatable, `D` defer, `R` recapture, `←/→` navigate, `T` typed anchors, `?` ask AI, `S` skip plugin, `⌘↵` submit
- Live progress: rows done, rows remaining, elapsed, estimated remaining
- **Three skip outcomes**, each writing a persisted record with a reason, never a silent absence

**Gate.** Map a compressor end to end in Fast mode in under 30 s, and a 5 band EQ in under 90 s, both measured with a real stopwatch on a real plugin, not estimated. The handoff's own rule applies: every unmeasured estimate in this project collapsed, usually smaller. If the target is missed, report the real number and adjust the target rather than the measurement.

> **Gate record (2026-07-31, real stopwatch).** API-2500 (m), 15 rows, list-era UI: **286 s** — failed on comprehension, and drove four usability passes (question strip, on-screen legend, queue rendering, the wizard). C1 comp (s), 9 rows, wizard, cold, **no proposals**: **89 s, ~10 s/row**, including one real error caught (threshold_db and knee_db both landing on [7] — now queried at capture, not at review). The 30 s target was written for the **proposal-backed lane**, which covers 26% of machine-relevant plugins; the bare lane is the 74% case and measures ~10 s/row. Per the gate's own rule the target adjusts, not the measurement: **bare-lane target = rows × 10 s** until proposals or better corroboration bring it down; the 30 s compressor claim stands only where a verdict exists for the fp.

---

### M5: Groups, families, twins

**Purpose.** The AMEK fix. Group membership does the work, range is a tiebreaker.

**Build**
- After band 1 is mapped, pattern infer the remaining bands from parameter names (`LF Gain 1` → `LMF Gain 1` → `MF Gain 1`, `band1 freq` → `band2 freq`). Present as a proposal table, one keypress bulk accepts, any single band correctable by wiggling.
- **Family tags** when more than one band family exists (E2Deesser's `sband` and `vband`). Prompt when the inference finds two disjoint name patterns.
- ~~**Channel twins** as `indices[]` from M2's correlated multi-move detection~~ *(retired: zero confirmed twins in the corpus; the one live lockstep pair — AMEK's Param Link channel mirror — is resolved by a human PICK on the band card, recorded as co-moved, never asserted)*
- `freq_range` per band from that band's freq anchors, free
- **The critical assertion, tested explicitly:** a parameter not in the group can never be selected by the band matcher regardless of its range. Test with AMEK: `Mono Maker` must be unreachable for a 250 Hz request even though its range covers 250 Hz.

**Gate.** Map AMEK EQ 200. Confirm `freq_hz` resolves to a real band and not index 7, `gain_db` to a real band and not index 3. Run 250 Hz and 8 kHz through the shared band matcher and confirm LF and MF respectively, neither touching the other's half of the spectrum, and `Mono Maker` untouched in both cases. This single plugin is the acceptance test for the whole project.

---

### M6: Tier 2 named controls

**Purpose.** Remove the vocabulary ceiling. `spiff`'s `sharpness` and `cut depth` become dialable without a schema change.

**Build**
- Every swept parameter not claimed by a Tier 1 semantic is recorded as a named control: real name (case sensitive, per the settled decision), anchors, range, unit if the display parser found one, `index` or `indices`
- **Duplicate names refuse to resolve**, same rule as Tier 1. `Bypass` and `bypass` are distinct. Two identical names both get recorded with a `duplicate: true` flag and neither is resolvable by name.
- Trust per control: `setread` in Fast mode, `human-verified` in Deep mode
- Schema addition to the v2 map format, `controls: { "<name>": {...} }`, already sketched in the spec

**Deferred decision (not blocking):** whether the chain prompt receives a per plugin control list when a Tier 2 map exists. That is a token cost on every chain turn plus a contract rule ("only name controls in this list"). **Store it now, expose it later.** Building the data is cheap and reversible; changing the chain prompt is neither. Nothing in M6 depends on that decision.

**Gate.** Map `spiff`. Confirm `sharpness`, `cut depth` and `boost depth` appear as named controls with anchors, and a synthetic `{"sharpness": 6}` resolves by name through the shared apply path to the correct normalised value.

> **Gate closed 2026-07-31 (human run): 35 controls, sharpness → [14], cut depth → [2], boost depth → [10].** The original wording also demanded `sensitivity` as a named control; on the human run it was ABSENT from controls **because it was already confirmed as a Tier 1 semantic at [13] — which is correct**: the controls sweep skips Tier-1-claimed indices by design (one index, one owner, and Tier 1 outranks Tier 2). A dial-set semantic that got claimed during assignment is a better outcome than a named control, not a miss. The gate wording is corrected so the next reader does not fail a correct result.

---

### M7: AI assist

**Purpose.** The human is fast at observing and slow at reading a 60 row parameter table. Invert that.

#### 7.1 Provider interface

```cpp
struct AssistProvider {
  virtual AssistResponse query(const AssistRequest&) = 0;
};
// AnthropicDirectProvider: local key, phase 1
// EchoJayBackendProvider : /api/ejmap/assist, phase 4
```
Key from macOS Keychain or `EJMAP_ANTHROPIC_KEY` env var. **Never a file in the tree.** This repo's sibling ships its working tree on deploy and loose files at deploy time have bitten three times.

#### 7.2 Context payload

Static per plugin, sent once and **prompt cached**:
- identity (format, uid, name, vendor, version, param_count)
- full parameter table: index, name, label, discrete, num_steps, default_norm, sweep points
- the classifier's existing verdict and confidence per row
- the category dial set being targeted

Dynamic per turn:
- what is mapped so far, what is deferred, what the human just asked
- screenshots (7.3)

Caching matters: the parameter table for a 60 param plugin is the bulk of the prompt and it is identical on every turn for that plugin. Cache it, same discipline as the chain path.

#### 7.3 Screenshots

*(Superseded by measurement, M2: `createComponentSnapshot` returns a BLANK
image for every editor tried — native plugin views do not composite into JUCE
snapshots. The working mechanism is window-level capture of OUR OWN window,
which works even occluded or 60% offscreen and returns nil when minimised —
the nil is reported, never papered over. Overlays draw BEHIND native editor
views, so the "labelled render" cannot pin labels over the plugin GUI; labels
composite onto the captured IMAGE instead.)*

- Window-level capture of the hosted editor's window, PNG, base64
- Tight crop at a given `ui_hint`, 3x the hint box, for "what does this knob read"
- A **labelled** render composited onto the captured image (not the live GUI), used for the approval sheet and shipped in the evidence

#### 7.4 Query modes

**Chat.** Free text, streamed into a side panel. "Which of these fifteen frequency parameters is the main band 1 frequency?" with the table and the screenshot attached.

**Structured proposal.** The model returns JSON only, no preamble, no fences, parsed into proposal rows the human confirms or rejects. Used for: initial dial set proposal, band pattern inference sanity check, family split detection, typed anchor pre-fill from a screenshot reading.

**Disambiguation.** Triggered on ambiguity: two or more parameters claim the same semantic, or the classifier said `unsure`. The model sees the crop and the table and proposes a ranked answer with reasoning.

#### 7.5 Rehearsal

After the human approves the map:
1. Generate (or use a canned per category) realistic settings block, the kind the chain would actually emit: `{"freq_hz": 250, "gain_db": -3, "q": 1.2}`
2. Apply through the **real** `applySettings`, not a harness
3. Capture before and after screenshots
4. Run the audio probe delta (M9)
5. Present all three to the human as final sign off

This is the closest available answer to "would EchoJay do the right thing with this map". Note the handoff's own warning: the client gate had 35/35 in a harness and wrote +16 dB in Logic. The rehearsal must use the shipping path.

#### 7.6 Cost control and an honesty rule

- Local token counter per plugin and per session, displayed
- Hard per plugin cap, configurable, default generous for Deep mode
- **Do not trust the model's self reported confidence.** Measured across the gold set: 1,276 high, zero unsure, no information. Where a confidence signal is needed, use disagreement under perturbation (ask twice with a varied prompt, flag mismatch), which did produce a useful triage list.
- Every AI proposal is a proposal. Nothing the model says writes to the map without a human keypress. `trust` never reads `human-verified` on a row the human did not observe.

**Gate.** On AMEK, with a screenshot and the table, the assist correctly identifies band 1 freq and gain. On a plugin with a Valhalla-class text liar, the assist reads the GUI value from a crop and pre-fills a typed anchor that the human confirms unchanged. Confirm the cache is hitting (token counter shows the static block charged once per plugin, not once per turn).

---

### M8: Approval sheet

**Purpose.** One screen, everything, before anything leaves the machine.

**Build**
- Row per mapped semantic and per named control: semantic, index or indices, real parameter name, kind, anchor sparkline, method, trust, probe verdict, `ui_hint` thumbnail crop
- Rows for every skip with its outcome and reason
- Rows for the noise mask and any `duplicate: true` collisions
- The labelled full editor screenshot, inline *(composited onto the captured image — overlays cannot draw over native editor views, measured in M2)*
- Per row approve, reject, or re-open
- **Submit is blocked** while any probe verdict reads `contradicts` and is unresolved
- Warning banner (not a block) when a category bar is not cleared, showing which semantics are missing

**Gate.** Produce a sheet for a mapped 5 band EQ and confirm every band, every twin, every skip and every named control appears. Confirm a deliberately mis-mapped parameter (map `gain_db` to a frequency control on purpose) produces a probe contradiction and blocks submit.

---

### M9: Audio probe

**Purpose.** Prove semantics from the signal. This is the layer that catches Mono Maker with no human present, and the largest single build item in the plan.

#### 9.1 Render harness

- Offline `processBlock` through the hosted instance, 48 kHz, 512 blocks
- Prepare, prime with 500 ms of silence to settle, then render the probe signal
- **Bypass reference:** render the same signal with the plugin's own bypass engaged, or with a fresh instance at defaults, whichever the plugin supports. Every measurement is a delta against the reference, never an absolute.
- **Sanity gate first:** if the plugin does not alter the signal at all under any parameter setting, every probe verdict is `inconclusive` and says so. A probe that reports "confirms" on a plugin that is doing nothing is precisely the false confidence class this project keeps producing.

#### 9.2 Per category suites

| Category | Signal | Measurement | Proves |
|---|---|---|---|
| **eq** | log sweep or pink noise, 16k FFT, Welch average | transfer function vs reference | `freq_hz` moves the peak/dip centre, `gain_db` changes its depth in dB, `q` changes its −3 dB width |
| **compressor / limiter** | stepped sine or noise, −40 to 0 dBFS in 2 dB steps | input vs output level curve | `threshold_db` moves the knee, `ratio` changes the slope above it, `makeup_db` shifts the whole curve |
| **comp envelope** | burst (silence, tone, silence) | envelope of gain reduction | `attack_ms` changes onset time constant, `release_ms` changes recovery |
| **gate** | same stepped signal | inverted curve | threshold moves the gate point |
| **de-esser** | band limited burst at 6 to 8 kHz plus a low band | per band reduction | `freq_hz` moves the reduced band, `range_db` / sensitivity changes the depth |
| **saturation** | single sine at −12 dBFS | THD and harmonic profile | `drive` raises THD monotonically |
| **delay** | impulse | tap positions in samples, repeat count | `delay_time_ms` moves the tap, `feedback_pct` changes repeat decay |
| **reverb** | impulse | RT60 via Schroeder integration, first reflection time | `decay_s` changes RT60, `predelay_ms` moves first reflection. **Noisy on modulated tails, verdicts are weaker here.** |
| **mix / wet-dry** | any of the above at 0% and 100% | null test against dry | `mix_pct` blends monotonically |
| **mode / enum** | none | none | **structurally unprobeable.** Always `inconclusive`. |

#### 9.3 Verdicts

Three, never two:
- `confirms`: the measurement moved in the predicted direction by a predicted magnitude
- `contradicts`: the measurement moved in the wrong direction, at the wrong frequency, or not at all when it should have. **Blocks submit.**
- `inconclusive`: the suite cannot reach this parameter, the plugin failed the sanity gate, or the measurement noise exceeded the threshold. **Never renders as a pass.** Displayed in a distinct colour, recorded in the payload, counted in the summary.

An inconclusive that reads as a pass would be the seventh instance of the silent drop class.

#### 9.4 Scope honesty

The probe is decisive on eq, compressor, limiter, gate, de-esser, delay and saturation. That covers, from the local numbers, 92 + 64 + 19 + 5 + 8 + 14 + 24 = 226 of the 390 currently dialable products, and it is the exact set where a mis-mapping does audible damage. It is weak on reverb and useless on mode. It augments human visual confirmation and never replaces it.

**Gate.** On a known clean EQ, sweep `freq_hz` across its range and confirm the measured dip centre tracks the requested frequency within 5%. On AMEK, map `freq_hz` deliberately to `Mono Maker` and confirm the probe returns `contradicts` (no spectral change at the requested frequency) without any human input. That is the headline test for this module.

---

### M10: Payload, local store, upload

**Purpose.** Get a verified map out of the tool with provenance that survives.

#### 10.1 Payload v2.2 *(2.1 as written; 2.2 is what shipped — ui_hint carries `editor_w/editor_h/screen`, readback entries carry `asked`, `captured_by` is `poll|poll+gesture|gesture|human_pick`, skips gained `mode_material`, controls entries carry `kind`/`labels`/`identity_display`/display-declared `unit`)*

The spec's payload, extended:

```jsonc
{
  "fp": "…",
  "schema": "2.2",
  "identity": { "format", "uid", "name", "vendor", "version", "param_count" },
  "category": "eq",
  "mode": "deep" | "fast" | "repair",

  "params": {
    "<semantic>": {
      "index": 3,               // or "indices": [3, 4]
      "param_name": "LF Gain",  // real name, case sensitive
      "kind": "gain_db",
      "anchors": [[0.0, -18.0], …],
      "trust": "human-verified",
      "method": "gettext" | "setread" | "human-typed",
      "ui_hint": { "x": 0.32, "y": 0.61, "w": 0.06, "h": 0.06 } // or null
    }
  },

  "controls": {                 // Tier 2
    "sharpness": { "index": 4, "range": [0,10], "unit": null,
                   "anchors": […], "trust": "setread" }
  },

  "groups": [ { "family": "sband", "n": 1, "primary": true, "freq_range": [lo, hi], "params": {…} },
              { "family": "sband", "n": 2, "freq_range": [lo, hi], "params": {…} }, … ],
  <!-- CORRECTED 2026-07-31: one group object PER BAND, n = the band NUMBER
       applyBands sorts and labels by. The previous example wrote one group
       with "n": 5 meaning five bands; applyBands reads n as a band number,
       so building against that example produces a single band called
       "sband5". The example was the source of the error (GroupSpec's struct
       comment copied it), and no served map has ever carried a group, so
       nothing downstream ever caught it. -->

  "skips": [
    { "semantic": "makeup_db", "outcome": "not_present", "reason": "…" },
    { "semantic": "q",        "outcome": "not_automatable", "reason": "…" }
  ],

  "evidence": {
    "captured_by": { "<semantic>": "poll" | "listener" | "both" },
    "noise_mask": [12, 13, 40],
    "readback":   { "<semantic>": { "wrote", "read", "match": true } },
    "visual_confirmed": [ "<semantic>", … ],
    "audio_probe": {
      "<semantic>": { "verdict": "confirms"|"contradicts"|"inconclusive",
                      "measure": "peak_freq_hz", "expected": 250,
                      "observed": 248.4, "note": null }
    },
    "rehearsal": { "settings": {…}, "applied": [...], "probe_delta": {…} },
    "screenshots": { "labelled": "sha256:…", "full": "sha256:…" },
    "assist_turns": 3,
    "duration_s": 84
  },

  "provenance": {
    "tester_id", "machine_id", "ejmap_version", "extractor_version",
    "apply_header_sha", "plugin_version", "host_os", "at"
  }
}
```

Two notes. `apply_header_sha` pins which version of the shared apply logic verified this map, so a later behaviour change is traceable. Environment travels with evidence, never with identity, per the settled decision.

#### 10.2 Local store

`~/Library/ejmap/` with `maps/`, `screenshots/`, `ledger.json`, `queue.json`, `inflight.json`. Resumable: quit mid plugin, relaunch, resume at the same row. Everything written to disk on every state change, not on submit.

#### 10.3 Upload and server acceptance

- Human verified maps do not need consensus. One human observation outranks three deterministic agreements.
- Keep the corroboration counter for genuinely independent human maps from different machines. A disagreement between two testers is a signal, not a tiebreak.
- **Structural gate at the mouth:** schema validation, anchors sanitize clean, no flat sweeps, indices in range, no unresolved `contradicts`, claimed category bar actually clears (or the payload declares it does not). **Reject at the mouth, log the reason, never silently drop.**
- **Merge per key over llm-classified** (spec open question 3). Human where present, model elsewhere. Replacing outright throws away model coverage on parameters the human skipped.

**Transport constraint (LOCKED 2026-08-01, before any transport exists).** The artifact and the wire come from **one builder**. The dry-run file (`upload/<fp>.http`) is the only pre-server verification this tool has; any transport that composes its own request retires it. Therefore:

- The transport either **replays the artifact's bytes over the socket**, or it **records what was actually sent and diffs it against the artifact on every send** — and a mismatch is a refused send, recorded in the queue with the diff, even on a 2xx.
- `juce::URL::withPOSTData()` and every convenience call that composes headers out of sight is **out**. This is not a style preference: the byte audit found the convenience layer dropping the leading slash from the request path, dropping typed ports from the Host header, and re-escaping query strings — all invisible until the emitted bytes were read.
- The request stays **HTTP/1.1, pinned**. A client that negotiates h2 reframes the request and the byte diff stops meaning anything.

This is a constraint on M11, not an M11 design question. The TLS decision below chooses *how* to satisfy it, never *whether*.

**Gate.** Submit a map, then read it back out of the store and assert field by field against the local JSON. Assert against **stored** data, never against the emitted event. Stubbing the store is exactly how the tripwire bug survived its own test.

---

### M11: Queue, provenance, distribution

**Purpose.** Twenty testers, ten plugins each, a weekend.

**TLS: the options and what each costs (stated 2026-08-01, decided at M11 signing).** Byte-truth over a raw socket versus HTTPS through a library that composes its own framing is a real tension; it gets decided deliberately here, not settled by whichever is easier to write. All options below operate under the locked transport constraint in 10.3.

- **Option A — OS TLS stream, artifact bytes verbatim.** Network.framework (`NWConnection` with TLS) carries the connection; the tool writes the artifact's exact bytes into the stream and reads the raw response. Byte-truth holds **by construction, before the bytes leave**: the TLS plaintext *is* the artifact, no diff needed. Costs: a bounded piece of ObjC++ platform code (one POST, no redirect following — a redirect is a refused send, no retry logic beyond re-queue); we own response parsing and timeouts; certificate trust stays the OS default, never custom. macOS-only, which this tool already is.
- **Option B — curl subprocess, record-and-diff.** curl speaks TLS and HTTP/1.1 (`--http1.1` pinned); `--trace` records the pre-encryption request bytes and the tool diffs them against the artifact **after every send**. curl's own header composition (`User-Agent`, `Accept`, `Expect: 100-continue`) must be suppressed per-header, and any drift across OS curl versions is caught loudly by the diff. Costs: subprocess plus trace parsing; and the diff runs after the bytes left, so one nonconforming request reaches the server before it is caught — tolerable for maps, and the send is still recorded as failed verification even on a 2xx.
- **Option C — juce::WebInputStream / withPOSTData or any client with no record of sent bytes.** Cannot satisfy the constraint: nothing to diff, headers composed out of sight. **Out by 10.3.** Listed so it is ruled out in words, not by omission.
- **Option D — plaintext HTTP to a local TLS forwarder.** Keeps raw-socket byte-truth but plants a proxy on every tester's machine. The M11 gate is a non-Sean tester on a clean Mac with no terminal; a proxy config fails that gate outright. **Out.**

Recommendation carried into the signing: **A**, because verification-before-send beats verification-after-send and the code is bounded; **B** is the fallback if A's platform cost proves out of proportion during the build. The decision is a signature, not a default.

**Build**
- Transport per the locked constraint in 10.3: one builder for artifact and wire, byte replay or record-and-diff on every send, HTTP/1.1 pinned. TLS per the option signed above.
- Queue: priority list by real suggestion frequency descending, intersected with plugins installed on this machine, minus already human verified. Second order: prefer `dialable: false` (biggest delta), prefer thin categories (de-esser at 8, delay at 14, gate at 5).
- Impact display: "You have mapped 12 plugins, 340 users have these installed."
- Signed and notarized standalone app, same identity as EchoJay: `Developer ID Application: Sean Donoghue (8BT5F9B887)`, notarize profile `EchoJayNotarize`
- Sign in with the EchoJay account, reuse `auth.json` if present, otherwise browser OAuth. Server returns a tester flag. Without it: map locally, cannot submit.
- **Token issuance replaces `EJMAP_INGEST_TOKEN` (interim, 2026-08-02).** The ingest route authenticates via `X-EJMap-Token`, currently a single shared value from the environment because the only tester is Sean. M11 must issue per-tester tokens at sign-in: a shared secret across twenty testers cannot be revoked individually, and gives the server no way to attribute or retract one tester's submissions. Storage stays env or keychain, never a file in the tree.
- Anti-abuse: allowlist, structural gate, admin review queue for a new tester's first N submissions, disagreement flag holds both maps for review, bulk retraction by tester ID
- Swap `AssistProvider` to the backend implementation here (the key cannot ship in the binary)

**Gate.** A tester who is not Sean installs the signed build on a clean Mac, signs in, maps three plugins, and submits, with no terminal, no dev mode and no verbal instructions beyond a one page readme.

---

### M12: Regression on plugin update

**Purpose.** A new plugin version changes the fp. Do not make the human re-map.

**Build**
- On encountering an unmapped fp whose `format|uid` matches an existing product with a human verified layout: attempt **name based transfer** of the map. Names survived every version transition measured, including a `Bank` insertion that broke 339 indices.
- Re-sweep and re-verify **anchors only**, since anchors are format stable but not always version stable (Soundtoys PrimalTap taper change, freqtube OutGain range compression, Melda enum reorder)
- Re-run the audio probe suite, fully automated, no human
- If names do not match, or a probe contradicts, drop to Repair mode and ask the human for the affected keys only
- Result: `trust: human-verified` carries forward per key, with `transferred_from` recorded in provenance

**Gate.** Take a plugin with two installed versions, human verify version A, and confirm version B transfers with anchor re-verification and no human input, with the probe passing.

---

## 4. Build order

Each phase has a gate. Do not start the next phase until the gate passes with a measured number.

| Phase | Modules | Gate |
|---|---|---|
| **1. Mechanism** | M0, M1, M2, M3 | 20 plugins mapped locally, dial set only, JSON on disk. **Measure the real per plugin time** and publish it, not an estimate. |
| **2. Semantics** | M4, M5, M6 | AMEK maps correctly (`Mono Maker` unreachable). E2Deesser splits `sband` and `vband`. `spiff` produces four named controls. Valhalla completes via typed anchors. |
| **3. Proof** | M9, M8 | The AMEK mis-map returns `contradicts` with no human input. Approval sheet blocks submit on it. |
| **4. Assist** | M7 | Assist correctly disambiguates AMEK band 1 from a screenshot. Cache confirmed hitting. |
| **5. Ship** | M10, M11 | A non-Sean tester submits three maps from a clean Mac. |
| **6. Durability** | M12 | Version B transfers from version A with no human input. |

Phases 3 and 4 are ordered deliberately: the probe before the assist. The probe is the only layer that produces ground truth, and the assist should be evaluated against it, not the other way round.

---

## 5. Risk register

Drawn from the failure patterns in the handoff, each one having bitten at least once.

| Risk | Guard |
|---|---|
| **Silent drop** (six confirmed instances) | Every skip, rejection, inconclusive probe and structural reject writes a persisted record with a reason. Nothing is absent without being recorded as absent. |
| **Estimates collapse when measured** | Every gate demands a measured number against stored data. No target ships as a claim before it is stopwatched. |
| **Test passes, feature does not work** | Round-trip test runs against real produced maps, not fixtures. Rehearsal uses the shipping `applySettings`, not a harness. The client gate had 35/35 in a harness and wrote +16 dB in Logic. |
| **Shared header drift** | `ejmap-roundtrip-test` in the pre-commit hook, `kMapSchemaVersion` static assert in both binaries. |
| **Worktree and build collisions** (four hazards, all bit) | Dedicated worktree, dedicated build dir, no shared install path, version and git hash on screen. |
| **Probe false confidence** | Sanity gate first: a plugin that alters nothing produces only `inconclusive`. Verdicts are deltas against a bypass reference, never absolutes. |
| **Assist false confidence** | Self reported confidence carries no information (1,276 high, zero unsure, measured). Use disagreement under perturbation. No AI proposal writes without a keypress. |
| **In process crash loses a session** | `inflight.json` before instantiate, quarantine on relaunch, state written on every change not on submit. |
| **Local API key leaks** | Keychain or env var, never a file in the tree. This repo's sibling deploys its working tree. |
| **Human fatigue produces bad data** | Deep mode is opt in. Fast mode is the default. Per key trust means a tired session degrades a map's trust labels rather than corrupting it. |
| **JUCE cannot see input on a native plugin view** | `ModifierKeys::currentModifiers`' mouse-button bits update only from events JUCE's own peer receives, and on macOS even `getCurrentModifiersRealtime()` refreshes just the keyboard flags (`NSViewComponentPeer_mac.mm:302`). Every editor this tool hosts is a native NSView, so a grab on the plugin GUI — the one surface this tool watches — was invisible to JUCE-side mouse state. Consequence, measured: the mouse ring's prefer-the-grab rule silently never fired from pass two until 2026-07-31; the non-null `ui_hint` rows passed via the most-recent-sample fallback, which looked like the feature working. Guard: input state that must reflect the whole screen comes from the system (`CGEventSourceButtonState`, combined session state), never from JUCE's event-derived mirrors; and any JUCE convenience that answers a global-sounding question ("is the button down?") gets checked against its implementation before being trusted across a hosting boundary. Proven on real hardware: grab visible through a native Pro-Q 3 view, suppression held, and the no-grab run refused with CANNOT PROVE rather than passing vacuously. |
| **Failure modes derived from parameter names cannot describe GUI structure** | A parameter list says what a plugin exposes to a host. It does not say how many on-screen controls write those parameters, which is a different fact that only a GUI can answer. Two of this plan's specified failure cases were written from name patterns and dissolved when measured: **meters-as-parameters** (predicted typical; 73 of 1,074,600 parameters have both a readout name and a flat sweep) and **channel twins** (predicted common and cited as an already-fixed MC 77 bug; zero confirmed cases, and the cited precedent was itself an inference from a name list). Guard: **anything the corpus says about controls is a hypothesis about names.** State it as a hypothesis, name the plugin and the GUI observation that would confirm it, and do not write a gate row that only a name pattern has ever supported. A row nothing can satisfy blocks the milestone while looking like rigour. |

---

## 6. Decisions still open

Not blocking any module. Flagged for when the data exists to answer them.

1. **Chain prompt exposure of Tier 2 controls.** Token cost per chain turn, needs a contract rule. M6 stores the data regardless. Decide after 50 plugins have Tier 2 maps and the real control-list size is known rather than estimated.
2. **Windows.** The extractor core is cross platform, the AU registry walk is not. VST3 only for Windows testers. Phase 5 or later, and only if tester supply on macOS proves insufficient.
3. **Bar definitions.** "Other" is 1,199 products at 22% clearance falling back to a compressor shaped bar. Separate work, unblocked by this tool, and a perfectly mapped reverb with only decay and mix still fails a two-of-two bar.
4. **What the mapper feeds back to the classifier.** Every confirmation and correction is ground truth against a model verdict. That is the labelled dataset for the long tail nobody will hand map, and it needs an export path. Cheap to add once M8 exists.

---

## 7. What this does not fix

Stated so the tool is not oversold.

- **The long tail.** 1,500+ plugins, testers will map hundreds. The rest stay on classified maps.
- **Category bars.** Bar tuning is separate work.
- **The edit path.** Blocked on the `[AVAILABLE BUILTINS]` capability marker decoupling, unrelated to mapping.
- **Plugins with no automatable parameters.** If the control is not exposed, no amount of clicking captures it, and `not_automatable` is a permanent and honest result.
- **A release.** Nothing client side has reached users. Maps reach users through the server, which is why the standalone decision holds, but read-back, the band matcher, apply-time honesty and TTL still need the branch reconciliation.
