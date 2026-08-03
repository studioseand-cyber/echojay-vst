# M9 Proposal: Audio Probe — revision 3 (signed for build with carve-outs folded)

**Status:** revision 3 — signed for build (harness, suites, stakes,
interaction design) with two carve-outs folded in below, both grounded in new
measurement: the Waves dead-parameter census (run before build, as ordered)
retracted this proposal's own fourth-liar-class specimen and reshaped the
render-deafness rule. Decided items are incorporated: constants kept as
declared with the 20-live-map re-derivation; the case-1 consumer defect is
FILED on v2 as `DEFECT_LADDER_TOLERANCE.md` (commit 192e053), decoupled from
M9; restore-at-relaunch is automatic, never silent, worded at session start,
and a failed verified restore keeps the stake and blocks probing that plugin
until acknowledged. The Task 0 spike remains disposable, in the session
scratchpad, never linked into ejmap. Filed alongside:
`DEFECT_INFLIGHT_POSTLOAD.md` (ejmap-side, any post-load crash unattributed)
and `DEFECT_LADDER_TOLERANCE.md` (v2-side, 192e053).

**Supersession index** (plan M9 section → this proposal):

| M9 section said | Superseded by | Where |
|---|---|---|
| Bypass reference: plugin bypass or fresh instance at defaults | A/B same-instance pairs across the probed parameter, **under a per-suite excitation state derived from the map**; input-vs-output for the sanity gate, also under excitation | Task 1, Blocking 1 |
| Offline render assumed affordable (unstated) | Measured: every subject ≥ 33× realtime; worst full suite ≈ 10–13 s wall | Task 0 (g) |
| Impulse → latency | First-energy is wet-path onset, not latency (VintageVerb declares 0, emits at 1399). Alignment via A/B xcorr, declared as fallback, explicit `alignment_unknown` | Task 0 (e), Task 1 |
| Frequency gate "within 5%" | **Kept**, restated in log-ratio terms: confirm tolerance log2(1.05) ≈ 0.070 octaves on the ratio, absolute Hz reported separately | Task 3 |
| Verdict thresholds (unspecified beyond 5%) | 4σ_f from a measured A/A noise floor; **plus two declared hand constants**: 0.25·Δ_pred for magnitude features and 1.0 octave for wrong-frequency — both listed as challengeable, with a re-derivation plan | Task 3 |
| reverb weak | predelay strong, decay weak with numbers attached, no guesses | Task 2 |
| mode/enum none/none | activity evidence recorded, verdict permanently inconclusive | Task 2 |
| (implicit) writes reach the DSP when written | **Measured false on the bridge**: writes need the message loop to land at all (pump-until-getValue-confirms rule). Revision 2's second claim here — a confirmed property-accepts/render-ignores liar class — is **retracted**: the census's finer instrument showed the specimen live; the class stays hypothesized with zero specimens, governed by carve-out 1 | Task 0-B, carve-out 1 |

---

## Task 0: Measurements (spike, this machine, 2026-08-02)

Subjects: bridged x86_64-only, rendering out-of-process via
`AUHostingServiceXPC_arrow` under Rosetta (confirmed in `ps` during renders) —
**spiff**, **API-2500 (m)** (Waves shell: 604 of 622 bridged), **CamelCrusher**,
**dearVR pro**. Native universal — **AMEK EQ 200**, **FabFilter Pro-Q 3**
(default and linear-phase/max), **ValhallaVintageVerb**, **bx_limiter True
Peak** (Q6's lookahead/true-peak subject). Excluded and counted:
**Auto-Tune EFX**, instantiation refused (OS error 4097, license wall).

### (a) Wall clock, 5 s of 48 kHz stereo (ms), first vs subsequent

| Subject | Arch | 64 first/sub (n) | 512 first/sub (n) | 4096 first/sub (n) | spread @512 |
|---|---|---|---|---|---|
| AMEK EQ 200 | native | 151 / 152 (3) | 150 / 150 (5) | 151 / 154 (5) | 149.7–151.5 |
| Pro-Q 3 (default) | native | 2.6 / 2.6 (3) | 1.5 / 1.6 (5) | 1.9 / 1.8 (5) | 1.5–1.6 |
| Pro-Q 3 (LP/max) | native | — | 22.7 / 16.9 (5) | — | 16.3–18.6 |
| bx_limiter TP | native | 140 / 135 (3) | 128 / 127 (5) | 125 / 125 (5) | 124.9–129.5 |
| VintageVerb | native | 17.0 / 17.1 (3) | 16.9 / 16.7 (5) | 16.8 / 16.8 (5) | 16.5–16.9 |
| spiff | **bridged** | 126 / 120 (3) | 101 / 114 (5) | 88 / 87 (5) | 90.7–194.0 |
| CamelCrusher | **bridged** | 32 / 31 (3) | 8.1 / 8.3 (5) | 5.5 / 5.4 (5) | 8.1–8.6 |
| dearVR pro | **bridged** | 30 / 29 (3) | 7.0 / 7.0 (5) | 4.3 / 4.3 (5) | 6.9–7.0 |
| API-2500 | **bridged** | 51 / 49 (3) | 22 / 25 (5) | 18 / 19 (5) | 21.2–29.5 |

Realtime margins, measured, not a band: **33× (AMEK), 38× (bx_limiter TP),
43–57× (spiff), ~200–260× (API-2500), ~300× (Pro-Q LP/max, VintageVerb),
~600–900× (CamelCrusher, dearVR), ~3,300× (Pro-Q default)**. The conclusion —
offline render is affordable everywhere sampled — stands on the 33× floor.

**Q6, the heavy class, measured instead of projected:** Pro-Q 3 switched to
Linear Phase at Maximum resolution goes 1.6 ms → 16.9 ms per 5 s (10×, first
render 22.7 ms for the kernel build) — still ~300× realtime. bx_limiter True
Peak (intrinsic true-peak oversampling + lookahead): 127 ms per 5 s, 38×
realtime, block-size-insensitive, near-deterministic (642 denormal-scale
diffs at 2.3e-35), declares 320 samples latency and emits at 305 — declared
latency wrong by 15 samples on a native plugin. The heavy class lands inside
the same order as AMEK; no budget change.

### (b) Fixed per-call vs per-sample (least squares over three block sizes)

| Subject | Per call | Per sample | Note |
|---|---|---|---|
| AMEK | ≈ 0 | ≈ 630 ns | per-sample dominated (TMT) |
| Pro-Q 3 default | ≈ 0.2 µs | ≈ 6 ns | trivial |
| VintageVerb | ≈ 0 | ≈ 70 ns | per-sample |
| bx_limiter TP | ≈ 0 | ≈ 520 ns | per-sample |
| spiff | ≈ 9 µs | ≈ 360 ns | per-sample above 512 |
| CamelCrusher | ≈ 7 µs | ≈ 21 ns | per-call below 512 |
| dearVR pro | ≈ 7 µs | ≈ 17 ns | per-call below 512 |
| API-2500 | ≈ 8 µs | ≈ 87 ns | roughly even |

The bridge's render path costs **7–9 µs per call** — an order of magnitude
under the 50–80 µs property reads. The two XPC paths are not the same path.
Block size: at 512, bridged per-call overhead is ≈ 4 ms per 5 s. **512.**

### (c) Instantiation and reset

| Subject | Instantiate (ms) | reset+prepare (ms, n=5, min–max) |
|---|---|---|
| AMEK | 340 | 0.07 (0.07–0.10) |
| Pro-Q 3 | 91 | 0.009 |
| VintageVerb | 100 | 0.20 |
| bx_limiter TP | 533 | (default-profile run) |
| spiff | 3700 | 28 (26–29) |
| CamelCrusher | 451 | 26 (23–27) |
| dearVR pro | 1644 | 27 (25–29) |
| API-2500 | 648–1232 | 27 (26–27) |

Instantiation is paid before M9 begins (probe runs on the loaded instance).
Reset+prepare, ~27 ms bridged, is a per-render budget line in (g).

### (d) Determinism (2 s identical renders, identical fresh state)

| Subject | Bit identical | Max abs diff |
|---|---|---|
| Pro-Q 3, spiff, CamelCrusher, API-2500 | yes | 0 |
| bx_limiter TP | almost | 2.3e-35 (denormals, 642 samples) |
| AMEK | no | 3.9e-5 (≈ −88 dBFS, modelling noise) |
| VintageVerb | no | **0.34** (signal-level, modulated tail) |
| dearVR pro | output silent | — |

Three grades: exact, noise-floor, signal-level. Task 3's σ_f absorbs all
three without hand tuning.

### (e) Declared vs measured latency (impulse first-energy, 0.1% of peak)

spiff 4480/4480 exact; Pro-Q 3, CamelCrusher, API-2500 0/0 exact; **AMEK
declares 32, emits at 0**; **bx_limiter TP declares 320, emits at 305**;
VintageVerb declares 0, first energy 1399 — which is wet-path onset, not
latency, and is the measurement that kills first-energy as a latency method.
Declared latency: wrong on 2 of 7 measurable subjects, in both directions.
(bx_limiter's 15-sample undershoot is likely the 0.1%-of-peak first-energy
threshold catching the rising edge of the lookahead ramp before its peak —
an instrument artifact, not a second AMEK-class declaration error; the
harness's xcorr aligner is indifferent either way.)

### (f) Stability — 200 sequential **1-second** renders at 512 (render length stated)

| Subject | Completed | Total wall | Host RSS | XPC (arrow) RSS |
|---|---|---|---|---|
| API-2500 | 200/200 | 861 ms | 34.22 → 34.23 MB | **111,088 KB → 111,088 KB** |
| spiff | 200/200 | 3797 ms | 31.97 → 31.97 MB | (service shared; flat) |

Bridged plugin memory lives in `AUHostingServiceXPC_arrow`, measured there
this revision: byte-identical before/after the batch. XPC service alive
throughout. The prior revision's host-RSS-only claim was unsupported for the
bridged case and is hereby replaced.

**Both readers validated against real allocation before the no-growth claim
stands** (a byte-identical reading is what a correct null and a stale reader
both look like): host-side `ru_maxrss` moved exactly +64 MB when 64 MB was
allocated and touched in-process; service-side `ps` RSS of the arrow
instance moved (+64 KB) at the moment a new-code plugin (CamelCrusher) was
instantiated into it from this process. Caveat recorded: the arrow service
is a single SYSTEM-SHARED instance (same PID across host processes, survives
an unprivileged pkill), so its RSS can carry other hosts' noise; the
stability runs were performed with no other AU host active. A second Waves
product loading into the shell moved it by ~0 — a true null, the shell
already resident.

Quarantine: a render crash **escapes the existing protocol** —
`endLoad()` deletes `inflight.json` at load success. Filed separately as
`DEFECT_INFLIGHT_POSTLOAD.md`; M9's own stake is in Task 5 regardless.

### Task 0-B (Blocking 2): write-to-render propagation on the bridge, measured

Method: settle a parameter at 0.2, write 0.9, then render continuously and
find the first 512-block whose RMS reflects the write; variants with 0, 50,
200, 500 ms of message-loop pumping between write and render; `getValue()`
read back after the pump to separate the property plane from the render
plane. Native control first to validate the method (AMEK Output Gain:
reflects at block 3–4 = 32–43 ms, which is its own smoothing ramp).

| Subject / param | Pump 0 | Pump ≥ 50 ms |
|---|---|---|
| spiff `trim` | property lands AND render reflects at **block 0** | same |
| API-2500 `Thresh` | property does NOT land (getValue still 0.2), render unchanged | property lands, render reflects at **block 0** |
| API-2500 `Output` | property does not land | property lands (getValue 0.9)… **render unchanged for 3 full seconds** (max RMS delta 1%), playhead attached or not |
| API-2500 `Makeup` | same | same: property 0.9, render deaf |
| API-2500 `In` | same | same (though 0.2/0.9 may quantise to one label) |

Two findings, one per plane:

1. **Propagation itself is zero once the write lands.** No measured subject
   needed settle between a landed write and the render reflecting it (block 0
   everywhere). But **landing the write requires the message loop on the
   bridge**: with zero pumping the write never leaves the host process —
   `getValue` still reads the old value. The harness rule: after every write,
   pump the message loop until `getValue()` returns the written value
   (stable-read style, bounded at 500 ms); measured cost ≈ 50 ms on Waves,
   ≈ 0 native and on spiff. Budgeted in (g). The probe never renders an
   A-config thinking it is B.
2. **RETRACTED (by the census, this revision): the "fourth liar class"
   specimen was a measurement artifact.** Revision 2 claimed API-2500's
   `Output`/`Makeup` accept property writes and never change the audio. The
   census re-measured them with a finer instrument — max sample difference
   > 1e-5 between aligned deterministic renders, against revision 2's 15%
   RMS gate — and both are **live**: they are small-range stepped trims
   whose full effect sits under a 15%-RMS threshold. The hand threshold
   manufactured the phenomenon. The property-accepts/render-ignores class
   remains a hypothesis with ZERO confirmed specimens, and the carve-out
   rule below governs whatever deafness is ever actually observed. The
   lesson is recorded because it is the project's recurring one: the
   instrument's own threshold must be validated before its findings are
   believed — which is also why the RSS readers are validated in (f) now.

### (g) Per-suite wall-clock budgets, recomputed from Task 1's own stimuli

Per render: pre-roll 0.5 s + stimulus + reset (27 ms bridged) + write-settle
(≤ 50 ms measured, 500 ms bound). Renders per suite: 3 A/A (noise floor) +
5 values per probed parameter.

| Suite | Stimulus length | Renders (4-param map) | Rendered audio | Worst wall (bridged ~23 ms/s + overheads) |
|---|---|---|---|---|
| eq | 2 s pink | 23 | 57 s | **≈ 4–5 s** |
| compressor curve | 21 steps × 300 ms = 6.3 s | 18 | 122 s | ≈ 5–6 s |
| compressor envelope | 4 s bursts | 13 | 58 s | ≈ 3 s |
| compressor total | | 31 | 180 s | **≈ 8–10 s** |
| limiter | 6.3 s + 4 s | ≈ 24 | 140 s | ≈ 7 s |
| gate | 6.3 s + 4 s | ≈ 24 | 140 s | ≈ 7 s |
| de-esser | 4 s two-band | 18 | 81 s | ≈ 5 s |
| delay | 1.6 s impulse+tail | 18 | 38 s | ≈ 3 s |
| saturation | 2 s sine | 13 | 33 s | ≈ 2.5 s |
| reverb (predelay/mix) | 2 s | 13 | 33 s | ≈ 2.5 s |

The prior "≈ 5 s" figure assumed 2 s stimuli universally; the compressor
family is the honest worst at **≈ 10 s** (≈ 13 s adding the 500 ms settle
bound on every write, which no subject needed). Still affordable; the Task 4
projection line shows the plugin's own measured number, not this table.

---

## Blocking 1: Excitation state

**The defect this section exists to prevent, restated as the review found
it:** probe `freq_hz` on AMEK with that band's gain at 0 dB and the
difference spectrum is flat — Δ_pred clears 4σ_f, |Δ_meas| < σ_f, and the
should-have-moved branch fires `contradicts` on a correct map, on the
headline plugin, at its default state. The same hole declares a flat-at-
defaults EQ inert at the sanity gate.

**Per-suite excitation plans, derived from the map, never from names.** An
excitation plan is a set of (companion semantic → target value in semantic
units) that must hold before the probed parameter can express itself. The
companions are resolved **through the map only** — the same band's group for
grouped EQs, `map.params` for flat semantics — and driven through the REAL
apply path (`applySettings` / `interpolateAnchors`), the single code path
doctrine. Targets are computed from the map's own anchors (e.g. "75% of the
mapped gain range"), not absolute constants.

| Probed | Required companions (from the map) | Target |
|---|---|---|
| eq `freq_hz`, `q` | same-group `gain_db` (grouped) or `map.params.gain_db` (flat) | +75% of mapped boost range |
| eq `gain_db` | same-group `freq_hz` set to a known anchor point | mid-band anchor (places the lobe where predicted) |
| comp `threshold_db` | `ratio` | ≥ 4:1 equivalent from anchors |
| comp `ratio` | `threshold_db` | low (−75% of range) |
| comp `attack_ms`, `release_ms` | `threshold_db` low + `ratio` high | as above |
| comp `makeup_db`, `output_db` | none | — |
| gate `threshold_db` | none (stimulus spans levels) | — |
| de-esser `sensitivity`, `freq_hz` | none (two-band stimulus is the excitation) | — |
| delay `feedback_pct`, `delay_time_ms` | `mix_pct` | ≥ 50% |
| saturation `drive` | none | — |
| reverb `predelay_ms`, `decay_s` | `mix_pct` | ≥ 50% |

**When the map does not name the companion:** the suite for that parameter is
not honestly runnable. Verdict: `inconclusive`, reason
`excitation unavailable: map names no gain_db` — never a guess from a
parameter name (names are hypotheses; measured rule). And structurally: **the
should-have-moved contradicts branch is armed only when the excitation plan
was satisfied.** Unexcited, it degrades to inconclusive. That closes the
review's false-contradicts construction exactly: AMEK `freq_hz` with no
drivable band gain can never fire contradicts.

**Sanity gate, re-based:** input-vs-output **under the suite's excitation
state**, not at defaults. A flat EQ with its probe band driven to +9 dB
alters the signal or genuinely does not; only the second is inert. Reason
strings distinguish three cases, from measurement this session:
`license_wall` (instantiation refused — Auto-Tune EFX, OS error 4097),
`wrong_channel_config` (the declared bus layout cannot be satisfied),
`inert` (layout matched and satisfied, excitation applied, output still
identical/silent — dearVR pro: a 1-in/2-out mono→stereo spatializer, measured
silent even with a matched-width buffer; suspected license mute, recorded as
inert-under-offline-hosting, suspicion stated, not asserted).

**State restoration, with its own stake.** A probe batch mutates the live
instance the human just set by hand; a batch that dies mid-run leaving probe
values in the session is the silent-drop class wearing knob positions.

- Before the first parameter write of a batch:
  1. `getStateInformation` → `<root>/probe-state-<fp>.bin` (flushed).
  2. `probe-inflight.json` written through the ledger's writeThrough path:
     `{plugin_id, fp, suite, param, render_n, state_file, started_at}`,
     updated per render (cheap, same discipline as `beginLoad`).
- Normal completion: `setStateInformation(saved)`, then a spot verification
  (re-read two mapped parameters through `getValue`, compare against the
  session's resolved values); only after verification pass are the stake and
  state file deleted. Verification failure keeps both on disk and raises a
  loud row + card: "probe could not restore your settings; re-check before
  submit" — the session file itself was never touched (probes write only to
  the live instance, never to session state).
- Crash/kill mid-batch: relaunch finds `probe-inflight.json`. Restoration is
  **automatic and never silent** (decided): the session-start card says, in
  words, "a probe died mid-run at <time>; your settings were restored from
  the stake and verified". A restore whose verification FAILS keeps the
  stake and the state file on disk, says so on the card, records
  `probe_state_unrestored`, and **blocks probing that plugin until the human
  acknowledges the card** — probing from an unknown state is how a wrong
  baseline gets measured. Either way a probe-quarantine row is written
  (auto-release on binary change, per decided Q5).

---

## Carve-out 1: render-deafness is not contradicts

Revision 2 held two contradictory positions — finding 2 called deafness a
true contradicts, the false-contradicts inventory listed causes that excuse
it. They are the same phenomenon and merge here, under the census's evidence
that deafness has at least two causes with OPPOSITE correct verdicts: a dead
shell index (contradicts, correct and valuable) and mode/routing suppression
(a correct map falsely accused — auto-makeup suppressing `makeup_db`, a
sidechain detector left unfed).

**The rule.** Render-deafness alone returns
`inconclusive: possibly mode-suppressed`. It becomes `contradicts` only
after BOTH exclusions run and pass:

- **(a) the map's own mode entries and labels show no parameter in a
  suppressing state.** This is a map lookup, not a name guess: every mode
  entry's current label is read; any entry whose engaged state could gate the
  probed parameter (the map cannot say which — so ANY mode in a non-default
  engaged/disengaged state counts as unexcluded) blocks the promotion. Where
  the map carries no mode vocabulary at all, the record says
  `suppression_unknowable: map silent on modes` — silence is recorded as
  silence, never assumed clear, and the verdict stays inconclusive.
- **(b) the human's gesture evidence does not show the control live at that
  index.** A capture or listener row proving the GUI once drove this index
  means the index is wired; deafness then describes this hosting, not the
  map, and the verdict stays inconclusive with that reason.

**The three outcomes, as the card shows them:**

1. Deaf, suppressor candidate present in the map:
   `? makeup_db [11]: no signal change measured. The map lists "Auto Makeup:
   On" among its modes — suppression is the likely reading. Not a defect.
   (inconclusive: possibly mode-suppressed)`
2. Deaf, map silent on modes:
   `? makeup_db [11]: no signal change measured, and the map carries no mode
   vocabulary to check suppression against. Cannot distinguish a dead index
   from a suppressed one. (inconclusive: suppression unknowable)`
3. Deaf, both exclusions pass:
   `PROBE CONTRADICTS makeup_db [11]: no signal change across the full range,
   no mapped mode can suppress it, and no gesture ever showed this index
   live. Likely a dead index. W re-verify — shift+N insist (recorded) — D
   later.`

## Carve-out 2: excitation verified by signal

The should-have-moved branch armed on plan *satisfaction* was defeasible by
exactly the deafness above: an excitation write confirmed by `getValue()`
that never reaches the DSP leaves the band flat, and the branch fires on a
correct map. Satisfaction is therefore proven in the only honest plane:

- After applying an excitation plan, render ONCE and compare against the
  pre-excitation render (same stimulus, aligned). Output changed → plan
  verified; the suite proceeds and the should-have-moved branch is armed.
- Output unchanged → every parameter depending on that plan returns
  `inconclusive: excitation unverified`, the branch stays disarmed, and the
  record carries both the plan and the null measurement.
- Cost: one render per plan, not per parameter — the (g) budgets grow by one
  stimulus render per suite (≈ 2–6% of a suite; worst case compressor
  ≈ 10 s → ≈ 10.5 s).

The census's first subject is the standing specimen: C1 comp-sc's detector
listens to an unfed sidechain bus, so no threshold excitation can verify —
every dynamics verdict on it honestly reads `excitation unverified` instead
of six false contradicts.

## The Waves dead-parameter census (run before build, as ordered)

> **CAVEAT ADDED 2026-08-02, PENDING RE-RUN.** This census was run through the
> OLD `writeConfirm`, which serviced the macOS runloop only when a plugin's
> property plane was slow enough to trip its wait loop. On fast-property-plane
> subjects it dispatched nothing at all, so a written parameter could never
> reach the DSP (`juce_MessageManager_mac.mm:379`; fixed in `e428ee4` by an
> unconditional `runDispatchLoopUntil(1)`). Most of the ten products censused
> are fast-property-plane.
>
> **A render-blind write is indistinguishable from a dead index by this
> census's own liveness test**, and it pushes the rate the WRONG way — every
> such write reads as deaf. The measured 0.8% dead-index rate is therefore an
> UPPER bound that may be substantially inflated, and it is the number that
> decided the conflict card is an edge case rather than the primary
> interface. **That design decision is not safe until the census is re-run
> under the fix.** No conclusion below should be cited until then.

Ten products, full automatable parameter surface, burst stimulus (so
envelope-shaped parameters can express), write-confirm via pumped
`getValue`, liveness = max sample difference > 1e-5 between aligned
deterministic renders at 0.15 vs 0.85; every deaf continuous parameter
retested once per discrete parameter flipped to its opposite extreme
(single-flip suppression probe).

| Product | Params | Live cont. | Deaf cont. | Live mode | Deaf mode | Woken by flip |
|---|---|---|---|---|---|---|
| C1 comp-sc (m) | 14 | 3 | **6** | 1 | 4 | 2 (Monitor) |
| CLA-76 (s) | 9 | 4 | 0 | 5 | 0 | — |
| API-550A (s) | 13 | 1 | 0 | 7 | 5 | — |
| API-560 (s) | 14 | 11 | 0 | 2 | 1 | — |
| API-2500 (m) | 12 | 3 | 0 | 9 | 0 | — |
| DPR-402 (s) | 35 | 15 | **2** | 5 | 13 | 0 |
| Aphex Vintage Exciter (s) | 7 | 4 | 0 | 1 | 2 | — |
| Butch Vig Vocals (s) | 16 | 14 | 0 | 2 | 0 | — |
| NS1 (s) | 1 | 1 | 0 | 0 | 0 | — |
| Bass Rider (s) | 9 | 7 | **1** | 1 | 0 | — |
| **Total** | **130** | **63** | **9** | **33** | **25** | 2 |

Reading, with the explanations measured rather than assumed:

- **Deaf-continuous rate: 9 of 72 (12.5%) — but 8 of the 9 dissolve.** Six
  are C1 comp-sc, whose detector is an unfed sidechain bus (its Filter
  Freq/Q additionally woke when `Monitor` flipped) — routing/excitation, not
  dead indices. Two are DPR-402 band frequencies, gated behind band-enable
  combinations a single flip cannot reach (recorded `suppressors untested at
  depth 2`, not cleared). One — Bass Rider `Artifacts` — is the only
  unexplained candidate dead index in the sample.
- **True dead-index rate: at most 1 in 130 (≤ 0.8%).** Not material. Per the
  order's own criterion, no separate defect filing: the conflict card is an
  edge case, not the primary interface, and shipping EchoJay is **not**
  dialling dead indices at scale.
- What the census DID surface at scale is the deaf-mode column (25 of 58
  discrete params show no signal change across their extremes at defaults —
  overwhelmingly interacting gates, e.g. API-550A's band-enable steppers)
  and one product-class finding worth its own line: **sidechain-variant
  products (comp-sc and kin) will read near-globally deaf to any probe or
  dial that does not feed the sidechain bus.** That is a consumer-exposure
  note for the server's product metadata, not an M9 defect.
- Method note recorded for honesty: "deaf at defaults under single-parameter
  excitation" is the measured quantity; the census cannot distinguish deeper
  excitation dependencies (threshold-needs-ratio chains) from deadness —
  which is precisely why the carve-out rule refuses to call any of it
  contradicts without the exclusions.

---

## Task 1: Render harness

**Where it runs.** A dedicated probe worker thread. Renders under
`processLock`, exactly as the pump.

**Pump ordering — per render, not per batch** (revised: a batch-long pause
freezes editors for up to 10 s, which is an interaction defect, not a
footnote):

1. Message thread: `ProbeScope` RAII calls `host.pausePumpForMutation()` —
   `pumpEnabled=false`, then take/release `processLock`, draining the
   in-flight pump block.
2. Probe thread: ONE render inside `ScopedLock (processLock)`: `reset()`,
   `prepareToPlay(48000, 512)` (only ever inside the lock with the pump
   disabled — prepare racing processBlock is the SIGSEGV), pre-roll,
   stimulus, tail. All buffers are probe-thread locals.
3. Between renders: `resumePumpAfterMutation()` — the pump breathes, editors
   paint. Cost: one drained block per render, ≤ 1 ms bridged; at 31 renders
   that is ~30 ms of overhead for a UI that never freezes longer than one
   render (worst single render: compressor curve on AMEK ≈ 0.7 s).
4. Any exit path — completion, exception, abort key — unwinds the
   `ScopedLock` and the `ProbeScope` destructor restores the pump via
   `callAsync`. A hang inside a render trips `Watchdog::Scope`
   (`probe_render`, 30 s): process terminates with the probe stake flushed.

**Message-thread operations that take `processLock` and what they do during a
batch** (from the code): `pausePumpForMutation()` (drain-only, safe),
`unload()` (full stop; blocked until the current render's lock releases —
acceptable, unload is explicit), `submitMap`'s write-back verify and the
lockstep write-verify (both take the lock via the pause/mutate pattern).
While a probe is running, `keyValid` refuses sweep/typed/capture/submit
actions with the reason "probe running — Esc aborts", so these sites cannot
contend mid-batch; abort drains within one render.

**Write-settle (from Task 0-B):** after every parameter write, pump the
message loop until `getValue()` returns the written value; bound 500 ms;
timeout = the write never landed → the render does not run, the pair is
recorded `write_unlanded`, verdict inconclusive with that reason. A landed
write needs zero further settle (measured, block 0 everywhere).

**Playhead (revised — the harness states what it presents):** a static
`AudioPlayHead` reporting playing=true, 120 bpm, 4/4, time-in-samples and
ppq advancing exactly with rendered samples, attached before `prepareToPlay`.
Measured: none of the eight subjects required it; tempo-synced delays and
LFO-bearing plugins get a coherent transport rather than none, and
tempo-sync remains listed as a false-contradicts guard below.

**Stimuli** — as revision 1 (pink 2 s / stepped 6.3 s / bursts 4 s / impulse
+1.5 s tail / 997 Hz sine 2 s), all fixed-seed, all −12 dBFS nominal.

**Alignment.** A/B cross-correlation; declared latency as fallback only;
`alignment_unknown` state when the xcorr peak ratio < 2. Declared latency is
recorded but never trusted alone — measured wrong on AMEK (declares 32,
emits at 0) and bx_limiter TP (declares 320, emits at 305). **Latency change
between A and B** (Pro-Q processing-mode class): `getLatencySamples()` is
re-read after every write; if it differs between the pair, the delta is
recorded, alignment is per-render xcorr, and time-domain features carry the
uncertainty or go inconclusive.

**Reference strategy, tail/steady-state, non-determinism (σ_f), rates** — as
revision 1, with one amendment: every A/A and A/B render pair runs **under
the suite's excitation state** (Blocking 1), so σ_f is measured in the state
the verdicts will be judged in.

**False-contradicts guards** (revised — the missing cases, each with its
degrade path):

| Case | Guard |
|---|---|
| External sidechain selected | Suite-phenomenon check: if NO excitation configuration produces the suite's base phenomenon (no compression at threshold-min/ratio-max), every parameter in the suite is `inconclusive`, reason `suite phenomenon absent (external sidechain / detector routing?)` — never contradicts |
| Auto-gain / auto-release modes | Direction-agnostic activity is recorded; a Δ in the unpredicted direction with the phenomenon present is still `contradicts`, but the reason string names the possibility and the conflict card shows it; auto-modes named in the map as mode params are excluded from prediction entirely |
| Tempo-synced delay / LFOs | Coherent playhead supplied (above); delay taps that quantise to the grid are compared in log-ratio like frequencies, and a tap pinned to a musical division across A/B fires the same `excitation unavailable`-style inconclusive, reason `tempo-locked` |
| Latency changes between A and B | Re-read + per-render xcorr, above |
| Render-deafness of any cause | **Carve-out 1, below: never contradicts on deafness alone.** The census showed every observed deafness so far dissolves into sidechain routing, mode gating, or the instrument's own threshold |

**Tier 2 controls receive no probe, stated explicitly.** Three reasons: a
verdict needs Δ_pred, and Δ_pred needs a semantic — named controls carry
none by design; the render budget would multiply by the control count (spiff
35, AMEK 62 — a 10–15× blowup for no verdict); and the controls' integrity
story is already the anchor + write-settle + duplicate/lockstep discipline.
Mode-kind controls get the same activity record as mode params, nothing more.

---

## Task 2: Per-category suites

As revision 1 (eq / compressor / limiter / gate / de-esser / delay /
saturation tables of provable ✔ / partial ✚ / not-provable ✘; reverb honest;
> **ACTUAL PER-CATEGORY PARAMETER COVERAGE (2026-08-02), stated next to the
> claim so the gap is visible rather than inferred.** M9's scope claim is
> *decisiveness on seven categories*. What the built suites decide is
> narrower: **one to four parameters, on one subject each.**
>
> | suite | parameters decided | subject |
> |---|---|---|
> | eq | 3 — `freq_hz`, `gain_db`, `q` | AMEK EQ 200 |
> | compressor | 4 — `threshold_db`, `ratio`, `attack_ms`, `release_ms` | API-2500 |
> | limiter | **1** — `ceiling_db` | bx_limiter True Peak |
> | gate | **1** — `threshold_db` | SSL X-Gate |
> | saturation, de-esser, delay | not built | — |
>
> The gate is the specific example: it probes only its qualified target and
> has no attack, release or range coverage at all, so a passing gate suite
> must not be read as having cleared SSL X-Gate. The 226-of-390 figure below
> counts parameters the suites COULD decide by category, not parameters any
> suite has decided. **Expanding coverage is separate work and should be
> scoped as such**, not absorbed into the suite build: the categories are
> demonstrated, the parameter sets are not.

mode activity-only; coverage 226 of 390 unchanged and not inflated), with
three amendments from this revision:

1. Every suite runs under its Blocking-1 excitation plan, and each ✔ above
   is conditional on the plan being satisfiable from the map; otherwise that
   parameter is `inconclusive: excitation unavailable`, not silently skipped.
2. Frequency-family features (`freq_hz`, `low/high_cut`, de-esser band,
   delay tap) are judged in **log-ratio**, absolute Hz reported alongside
   (Task 3).
3. The compressor family's stimulus cost makes it the budget's worst case
   (Task 0 (g) table); the suite order runs cheap-first (eq before comp) so
   partial results appear early on the footer line.
4. **Delay suite gains one transport-stopped A/B** (playhead
   isPlaying=false): a tempo-synced delay that ignores ms writes while
   synced may free-run in ms when stopped, and the stopped pair reveals
   which parameter is live in which transport state. Recorded as
   `transport_dependence` evidence on the row; tap verdicts are issued only
   from the transport state in which the parameter proved live.

---

## Task 3: Verdict rules

Three verdicts; `inconclusive` is the default and never renders as a pass;
`blocksSubmit()` decides meaning. Sanity gate first, under excitation, with
the three-way reason vocabulary (Blocking 1).

**Frequency-family verdicts are on the log-ratio, absolute Hz separate**
(revised). Let r = log2(f_meas_B / f_meas_A) − log2(f_pred_B / f_pred_A):

- `confirms`: |r| ≤ log2(1.05) ≈ **0.070 octaves** — the plan's own 5% gate,
  kept, expressed where it is scale-free — AND the moved feature clears 4σ_f.
- `contradicts` ("wrong frequency", now a number): the spectral change is
  real (≥ 4σ_f) but its centre sits **> 1.0 octave** from prediction, or the
  ratio error exceeds 1.0 octave. Mono-Maker-as-freq fails this by construction
  (a low-shelf/mono change against a 250 Hz-vs-8 kHz prediction is octaves out).
- A **consistent ratio with a large absolute offset is not contradicts**: if
  |r| passes but absolute measured Hz disagrees with the anchor table's
  claimed Hz by a near-constant factor (e.g. ~1000×), the verdict is
  `confirms` on the ratio and the offset is recorded as a
  **display-unit-mismatch finding** — a liar-class instance against the
  display, exactly the kHz/Hz class. Acceptance case 2 restated below under
  this rule.

**Per-feature tolerance table** (replacing the single formula; every row is
max(stated tolerance, 4σ_f), σ_f measured per plugin per feature from the
A/A pair under excitation):

| Feature | Domain | Confirm tolerance | Source | 20-map re-derivation? |
|---|---|---|---|---|
| Frequency position (eq centre, cut corner, de-esser band) | log-ratio | 0.070 octaves (= the plan's 5%) | plan-stated, kept | no — signed gate |
| Delay tap time | log-ratio | 5% ratio error | same class as frequency | no |
| Gain depth (eq lobe, makeup, output, de-esser reduction) | dB | 0.25·Δ_pred | **declared hand constant** | yes |
| −3 dB width (q) | octaves | 0.25·Δ_pred | declared | yes |
| Knee curvature | dB | 0.25·Δ_pred (shallow knees expected inconclusive) | declared | yes |
| Slope above knee (ratio) | dB/dB | 0.25·Δ_pred | declared | yes |
| Attack / release τ | log-ratio | 25% ratio error | declared (same constant, ratio domain) | yes |
| THD (drive) | dB | 0.25·Δ_pred | declared | yes |
| RT60 (reverb decay) | s | 4σ_f dominates by design | measured floor | yes |
| Onset / predelay shift | samples | 5% ∨ 4σ_f | same class as tap time | no |
| Wrong-frequency contradicts gate | log-ratio | > 1.0 octave | **declared hand constant** | yes |

**The 0.25 constant's roles, enumerated (gate amendment 2) — it appears in
three distinct roles across seven uses, and the 20-map re-derivation must
move all of them together:**

| Role | Where it is used | Uses |
|---|---|---|
| R1 magnitude confirm tolerance, `\|Δ_meas − Δ_pred\| ≤ 0.25·\|Δ_pred\|` | gain depth, −3 dB width, knee curvature, slope above knee, THD | 5 |
| R2 time-constant confirm tolerance, 25% ratio error | attack τ, release τ | 1 |
| R3 **lobe-existence depth floor**, `depth ≥ 0.25 · expressed Δ_pred` (added by amendment 2; also the depth floor below which a lobe centre is UNDEFINED) | eq localized-absence test (gate A1), every centre extraction | 1 |

R3 is new and deliberately reuses R1's constant rather than introducing a
fourth number; the retired B5(ii) side-null would have been a fifth use and
was deleted rather than given a threshold.

The re-derivation (decided): the "yes" rows are recomputed from the
empirical distribution of |Δ_meas − Δ_pred|/Δ_pred over the first 20 live
probed plugins; until then they carry the label "challengeable, undata'd".
The frequency 0.070-octave gate is the plan's own signed 5% and is not
re-derived.

`contradicts` = wrong direction at ≥ 4σ_f, or (excitation **verified by
signal** per carve-out 2 AND Δ_pred ≥ 4σ_f AND |Δ_meas| < σ_f) — and for
render-deafness specifically, only through carve-out 1's two exclusions.
Everything else `inconclusive` with numbers.

Δ_pred always comes through `dominantMonotonicTable` + `interpolateAnchors`
— the ladder rule; no expectation from raw requests.

---

## Instrument floors and resolution (gate amendments 3 and 4, measured 2026-08-02)

`--gate-m9 instrument`, no plugin anywhere in the path. σ_f from a plugin A/A
pair captures **repeat noise only**, and a deterministic plugin has none —
AMEK measured σ_side = 0.000, which made 4σ a no-op. Every σ_f is now floored
at the extractor's own measured resolution, and **both numbers are reported
separately at every run**.

| Floor | Measured | Method |
|---|---|---|
| numerical | **0.000000 dB** | bit-identical input pair — the extractor adds no spurious noise, exactly deterministic |
| depth | **0.088 dB** | worst error extracting a known +6.00 dB synthetic peak, 40 Hz–16 kHz |
| centre, ≤ 250 Hz | **0.0322 oct** | worst error, known-truth synthetic peaks |
| centre, > 250 Hz | **0.0138 oct** | same |
| width | **0.0265 oct** | spread across independent noise realizations |
| side band | **0.4857 dB** | same |
| (realization depth, decorrelated case only) | 6.2038 dB max-bin | independent realizations; **not** used as a depth floor — the harness's fixed seeds make A and B share their realization, so this variance cancels. Recorded for the time-variant-plugin case |

**Centre resolution vs frequency, the amendment-4 measurement** (synthetic
peak, +6 dB, Q 1.0; FFT 8192, bin width 5.86 Hz):

| true Hz | measured Hz | error (oct) | error (%) |
|---|---|---|---|
| 40 | 40.4 | +0.0158 | +1.10 |
| 63 | 63.7 | +0.0152 | +1.06 |
| 80 | 79.8 | −0.0028 | −0.19 |
| 100 | 100.4 | +0.0051 | +0.35 |
| 160 | 163.6 | +0.0322 | +2.26 |
| 250 | 251.3 | +0.0077 | +0.53 |
| 400 | 403.8 | +0.0138 | +0.96 |
| 630 | 626.9 | −0.0071 | −0.49 |
| 1 000 | 998.5 | −0.0022 | −0.15 |
| 2 000 | 1 994.2 | −0.0042 | −0.29 |
| 4 000 | 4 009.0 | +0.0032 | +0.22 |
| 8 000 | 7 980.8 | −0.0035 | −0.24 |
| 16 000 | 16 007.5 | +0.0007 | +0.05 |

**The choice, made from the measurement:** parabolic peak interpolation (already
in the build) is sufficient — no larger FFT, no low-frequency special case.
The review's premise that the estimator is "~12% at 100 Hz" was the raw-bin
figure; **measured with interpolation it is 0.35% at 100 Hz**, and the worst
case anywhere is 2.26% at 160 Hz, against a 5% gate. Resolution is folded
into σ_centre as a frequency-dependent floor (0.0322 oct ≤ 250 Hz, 0.0138 oct
above) for the **expressibility** test, where σ is doing its actual job.

**A finding this measurement produced, which changes how the B1 number should
be read:** the gate's arm B measures AMEK's "100 Hz" band centre at
**94.6 Hz** (−0.079 oct from the anchor ladder's claim), while the instrument
at a true 100 Hz reads 100.4 Hz (+0.005 oct). **The 0.028 oct B1 error is
therefore plugin-and-map truth, not instrument error** — AMEK's filter really
is centred below where its anchor table says. B1 spends its tolerance budget
on the plugin's own geometry, not on estimator coarseness.

**Centres are now undefined, not estimated, below a depth floor.** A lobe must
clear `0.25 · expressed Δ_pred` to have a centre at all; below it the feature
reports `centre UNDEFINED` and no verdict may cite a centre. In the gate's
arm A this is what stops a 0.235 dB broadband ripple from carrying a centre
that could have landed near 400 Hz and produced a right-looking wrong verdict.

---

## Task 4: Interaction design

As revision 1 (probe runs post-resolution in the background + `P` on demand;
conflict card for contradicts-vs-human with W / shift+N insist / D, submit
refused while a conflict is open; inconclusive glyph never green; skips
recorded), amended per decisions and review:

- **Auto-run always** (decided Q3): no 30 s constant. The footer line shows a
  live projection from the plugin's own first measured render, and **Esc
  aborts within one render** (the per-render lock means abort latency is one
  render, worst ≈ 0.7 s). An abort records `probe_aborted` with progress.
- **Editors keep painting**: the pump pauses per render, not per batch
  (Task 1). The footer says `PROBE: rendering 14/31 — editor may stutter
  briefly`. No multi-second freeze exists to explain away.
- The conflict card for the dead-parameter class carries the Task 0-B
  evidence line ("property confirms 0.9; render deaf for 3 s") so the human
  decides with the actual measurement in front of them.

---

## Task 5: Loud failure

As revision 1's table (probe stake, watchdog site, NaN guards, stale-running
rendering as FAILED, append-only records as session cargo, silent-plugin and
no-suite rows), plus this revision:

| Failure | What disk says |
|---|---|
| Batch dies with probe values in the live instance | `probe-state-<fp>.bin` + `probe-inflight.json` (Blocking 1): relaunch restores the human's settings and says so, or says it could not, on a card and in the ledger — never silent knob drift |
| Write never lands (bridge, message loop starved) | `write_unlanded` pair record; render skipped; verdict inconclusive with reason |
| Property accepts, render ignores | liar-class instance record (plane disagreement, with numbers) + true contradicts on the conflict card |
| Restoration verification fails | stake and state file retained on disk; loud card; `probe_state_unrestored` ledger row |

The `endLoad()` finding is filed independently as
`DEFECT_INFLIGHT_POSTLOAD.md`: every post-load crash is unattributed today —
sweeps, captures, submit verify, not just renders. M9's stake closes the
render window only; the defect document proposes the general fix and its
trade-off for decision.

---

## Acceptance gate: the three defects, scored by WHERE the catch happens

**1. q reverting on a float ulp.** The review is right: **this is a
dial-path defect in the consumer, and ejmap's tolerance rule cannot reach
it.** The revert happened in EchoJay's dial-time readback, whose tolerance is
consumer code in `Source/EchoJayParamApply.h`'s callers — not in ejmap. What
M9 contributes at map time: the anchors already carry the ladder, the probe
verifies the ladder's effect (eq width feature via ladder-aware Δ_pred,
verdict `confirms`), and the map is therefore evidence that 0.7-between-steps
lands on 0.8 by design. The fix that actually kills the defect — a
ladder-aware dial-time tolerance derived from the map's own anchor spacing —
is a v2 consumer change, same filing lane as `DEFECT_BRIDGED_READBACK.md`.
**Not caught by M9; made fixable by M9's evidence; consumer filing needed.**

**2. "1.2 kHz" vs 1200 Hz.** **Caught in ejmap at map time**, under this
revision's log-ratio rule: the probe measures the lobe ratio (correct →
`confirms`) and the ~1000× absolute offset against the display-claimed Hz is
recorded as a display-unit-mismatch liar instance. Per decided Q1, the probe
verdict outranks the parse **for the revert decision only** — the correct
write survives, the disagreement is recorded as a display liar, and nothing
probe-derived enters anchor construction. At dial time the same class in the
consumer still needs the display-unit discipline the maps already carry
(display-declared units) — outside M9, already shipped in 2.2.

**3. 8 kHz on a 780 Hz-ceiling band, clamp confirmed as success.** **Not
caught at dial time by M9, and cannot be: the defect is in the consumer's
answer, not the map.** At map time the probe verifies the measured reachable
range (lobe at norm 0 and norm 1, signal-verified) into
`evidence.audio_probe` — which, per decided Q4, carries nothing
authoritative until the server round-trip test proves the field survives
ingest and returns from Redis. Dial-time honesty ("clamped to 780, by 3.4
octaves") is a consumer/server change consuming that evidence. **Score
unchanged and stated plainly: M9 itself catches one of the three (case 2);
cases 1 and 3 are consumer-side defects M9 arms but cannot fire.**

---

## Future work, recorded not built: the over-claiming-scale defect class

> **Same caveat, lesser exposure (2026-08-02).** Measured through the old
> `writeConfirm` too, but far less exposed: two independent estimators sharing
> no machinery agreed (44% and 47%), and gain reduction is a DIFFERENCE of two
> readings taken in the same convention, so a systematically missing write
> would have to affect both estimators identically to preserve the agreement.
> Re-run alongside the census, but the finding is expected to hold.

**A third defect category, alongside dead indices and mode suppression.**
Measured on API-2500 (2026-08-02): its threshold ladder delivers **~45% of
nameplate** — walking the ladder 15 dB moves the actual compression onset
about 6.4 dB. Confirmed by **two independent estimators sharing no
machinery** (knee location via two-segment fit: 44%; gain reduction at
three fixed probe levels: 47%), with the ratio confounder excluded by
recomputing against the *measured* 7.04:1 rather than the nominal 10:1.

The shape of the defect: **the map is correct** — the index is right, the
anchors are right, the write lands — **and the plugin's own scale
over-claims**, so a dialled value lands well short of what the user asked
for. Neither a dead index nor a suppressing mode; nothing in the parameter
list, the display, or the readback can reveal it. **Only a probe that
measures EFFECT rather than POSITION can see it**, which is the argument
for M9 stated in a single measurement.

The method that found it is the one to reuse: hold the confounding
parameter fixed, walk the ladder across ≥4 points, measure the effect at
fixed probe levels, and require two estimators to agree before attributing
the shortfall to the plugin rather than the instrument.

The consumer-side question — what a dial-time layer should DO when a map
is correct but its scale under-delivers — is out of M9's scope and needs
its own decision.

**Observation, two specimens: empty unit families on the parameters that
matter most.** API-2500 "Attack" (Waves shell) and SSL X-Gate
"Lower Threshold" both sweep with unitFamily EMPTY -- on two different
vendor shells. The display-declared unit path is therefore unreliable on
precisely the parameters where MAGNITUDE verdicts are wanted (times and
thresholds), which is why both currently return inconclusive-with-reason
rather than a scaled comparison. Recorded, not yet acted on.

**Level convention is declared at every level number** (peak / RMS / true
peak) after the 3.01 dB audit: the stepped stimulus is a sine PEAK
amplitude, so steppedCurve now converts its RMS reading to peak
(+3.0103 dB) and every curve number is dBFS PEAK. The mismatch was never
limiter-only -- it offset the knee estimator too.

## WHEN A GUARD CANNOT SIT AT THE CHOKE POINT, THE CHOKE POINT IS DOING TWO JOBS

**Amendment to the misplaced-guard entry: the fork's placement was a
structural collision, not an omission**, and that is a different lesson
from the other instances.

`routeVerdict` could not be moved onto `crit()` because `crit()` served
**two populations**: 11 harness assertions (the headline gate testing
*itself* against a fixture) and 8 parameter verdicts (claims about a
plugin). Forcing a harness assertion through the fork is meaningless —
there is no Δ_pred for *"the stake restored five parameters"* — so any
attempt to put the guard at the choke point would have broken the gate's
own self-test. The guard was pushed out to a convention because the choke
point **could not accept it**, not because anyone forgot.

**The diagnostic:** if a guard "obviously belongs" somewhere and resists
being put there, stop trying to place the guard and look at what the site
is doing. A function that two kinds of caller use for two kinds of purpose
cannot enforce a rule that applies to only one of them.

**The fix is to split the site, not to relocate the guard:**
`assertHarness()` for assertions, `emitVerdict()` for verdicts with the
routing inputs as *required arguments*, and `emitInconclusive()` for
inconclusive-by-precondition — structurally unable to express a confirm,
since no branch, parameter or return value in it can produce one. After
the split the guard sits where it always belonged, and enforcement is by
signature rather than by convention: a suite that omits the routing inputs
does not compile.

## NAMING A CLASS DOES NOT PREVENT IT (eighth instance)

The misplaced-guard class had been named, written up, and sat in this
document when the **newest suite reproduced it anyway**. Saturation calls
`crit()` directly instead of `routeVerdict()`, so a null that belongs in
carve-out 1 (`inconclusive: possibly mode-suppressed`, with `[8] Master XL
On` as the named suppressor candidate) was emitted as a FAIL against the
plugin.

**What makes this instance different from the seven before it:** the rule
was not missing, forgotten, or undiscovered. It was written down, recent,
and known to the author at the time of writing. Documentation did not
prevent the defect, and there is no reason to expect the next write-up to
do better.

**The audit that followed is the real finding.** Of **19 verdict-emitting
sites, only 3 route**:

| suite | verdict | routes? |
|---|---|---|
| limiter | `ceiling_db` | yes |
| gate | `threshold_db` | yes |
| saturation | `drive` | **no** |
| compressor | `threshold_db` (GR-at-level) | **no** |
| compressor | `ratio` | **no** |

So the compressor's headline results — the 45% over-claim and the ratio
confirm, both reported repeatedly — **have never passed through carve-out
1**. Had either been a deafness case, it would have been published as a
plugin finding. They may well be correct; they are simply unrouted, and
that is not the same thing.

**The conclusion the evidence supports:** a guard that suites are *supposed
to call* is not at a choke point, it is at a convention. The sensitivity
check and ambiguity rule fired unprompted in this same suite because they
sit in the shared **preamble** — code the suite cannot run without. The
verdict path needs the same property: `crit()` routes internally, or the
emit path collapses to one function that cannot be bypassed. **Only moving
the guard somewhere it cannot be skipped prevents the class.**

## A NAME IS NOT EVIDENCE OF IDENTITY (sibling to instance seven)

Instance seven says a name is not evidence of *behaviour*. Its sibling:
**a name is not evidence of identity.** Substring matching resolves the
first thing that contains the string, which is not the thing you meant.

Worked example, 2026-08-02. The saturation suite asked for
`"bx_saturator V2"` and got:

```
name 'UAD bx_saturator V2' | vendor 'Universal Audio'
id AudioUnit:Effects/aufx,33au,!UAD
```

A **UAD** plugin, not the Plugin Alliance one intended — same product
licensed through a different host, requiring UAD hardware, whose load
drowned the run in `UADLogger` output and tripped an editor-ready stall
sample. The suite reported nothing because it never reached its subject.

The resolver was exonerated in the same pass: `describeFromRegistry` reads
`AudioComponentFindNext` / `GetDescription` / `CopyName` — registry
metadata only, no instantiation — so the census walk is cheap and safe and
does **not** explain load failures elsewhere. The cost came entirely from
loading what the name matched.

**The rule:** a subject resolved by name must have its resolved identity
printed — name, vendor, format, id — *before* it is loaded, and a suite
naming a specific product should match on the id rather than a substring
of the display name. Verified identity is as much a precondition as
verified suitability.

### A refused operation can leave you checking the wrong thing

**Sixth instance, and it adds a mechanism the others did not have.** Verifying
the params work on a merged tree, `git checkout feat/preview-combined` was
**refused** — that branch was already checked out in a different worktree. The
checkout failed; the checks that followed did not. They ran happily against the
branch still checked out, reported `node --check` clean, both hazard counts
correct, and would have been reported as "the merged tree is good."

The tree they described was not the merged tree.

Same shape as the invented doc anchor — a verification that ran against the
wrong artefact and reported success — but with a new mechanism worth naming:
**a refused operation leaves you somewhere, and that somewhere is usually the
place you were trying to leave.** The failure mode is not "the check did not
run"; it is "the check ran, on the wrong thing, and passed." A command that
silently no-ops leaves nothing behind; a *refused* one leaves a plausible
context that answers every subsequent question confidently and wrongly.

**The practice that follows:** after any operation that changes *where* you
are working — checkout, cd, worktree switch — print the resulting location and
identity before trusting anything measured there. The merged-tree checks were
re-run only because the refusal happened to be visible in the output; had it
scrolled past, the report would have been false.

## UNBLOCKING A PATH PUBLISHES WHATEVER BROKEN CHECK IT WAS HOLDING (3 August 2026)

**A null is a check that has never been exercised.** While a suite returns
null, its verdict machinery is untested by construction: nothing it computes
has ever been compared against anything, and no output has ever been read. The
moment the blockage clears, whatever was sitting there publishes.

**When a suite stops returning null, re-read what its verdict compares before
trusting the first result it produces.**

**Instance 1: saturation's drive verdict.** It passed `thdMoved` as the
measurement and `jmax(1.0, thdMoved)` as the prediction — **the same number**.
`routeVerdict` compares `||moved| - |pred||` against a tolerance, so the
comparison was of a quantity with itself and the route was `tracks` for any
plugin whatever.

It survived the module's whole life because THD never moved: the suite drove a
stage whose amount was 0%, reported a null, and nobody looked at what the
verdict was comparing, because it never issued one. **Fixing the excitation is
what exposed the tautology** — the first genuinely excited run produced
`CONFIRMS ... moved 35.29 dB against 35.29 dB predicted`, and the two numbers
being identical is the tell.

*A check that cannot fail is not a check*, already recorded, with a new way of
arriving there: a blocked path holds a broken check indefinitely, and
unblocking the path publishes it. **When a suite stops returning null, re-read
what its verdict compares before trusting the first result it produces.**

The fix is not a better prediction — none exists. Drive is unitless, so
nothing predicts a THD magnitude from a drive value the way freq predicts a
centre or threshold predicts gain reduction. The magnitude claim is refused and
the falsifiable claim, that THD rises monotonically with drive, is decided on
its own. Same treatment comp's attack/release already gets under an undeclared
unit family.

---

## THE PASS: what else has been returning null or inconclusive (3 August 2026)

Run after saturation's tautology, on the reasoning that a long-standing null is
a long-standing unexercised check. **Reported, not fixed.**

**Checked and clean.** Every remaining `emitVerdict` site was read for the
saturation shape — a measurement passed as its own prediction. There are five,
and all five compare genuinely different quantities: limiter/gate (feature span
vs clipped ladder span), comp threshold (measured GR span vs span x ratio
factor), comp ratio (measured slope delta vs ladder-derived delta), comp
envelope (measured tau ratio vs ladder ratio), eq (measured feature vs ladder
prediction). **Saturation was the only tautology.**

**Finding 1: comp's envelope floor is an invented constant, and the verdict
that would use it has never issued.** `sgTau` IS measured, by A/A pair, and
printed — then discarded. The verdict passes `Floor(0.0001, "log2 ratio")`, a
hardcoded number, while the measured floor sits in milliseconds and is never
converted. attack_ms and release_ms have returned INCONCLUSIVE on every run
(API-2500's ladders declare no unit family), so the floor has never been
exercised. Same family as saturation's: a number nobody has checked, protected
from scrutiny by a path that never runs.

**Finding 2: carve-out 1's promotion is described in output and implemented
nowhere.** `routeText`'s deafness branch tells the operator *"promotion to
contradicts requires (a) no suppressing mode in the map AND (b) no gesture
evidence at this index"*. The limiter/gate suite gathers and prints both
exclusions when the route is deafness. **No code promotes.** Whether promotion
is meant to be automatic or a human judgement is not decided anywhere in the
source; the sentence reads as a rule the tool applies. The deafness route has
fired rarely enough that nobody followed the sentence to its implementation.
(The `promot*` matches elsewhere in the tree are EjmapCapture.h's noise
promotion, an unrelated mechanism.)

**Not in this class, but noted while looking:** `resolveSubjectByName`'s
harness-miss branch and eq's `B2 expressible` are both built, correct on
inspection, and have never fired — already M9 open item 6. A never-fired branch
is unproven rather than broken, but it is the same blind spot.

---

## AN ENABLE LINK AND AN EXCITATION STEP CARRY OPPOSITE NULL REQUIREMENTS (3 August 2026)

They share one type, `ExcitationStep`, because "set index 8 to 1.0 so index 7
is live" and "set ratio to max so the compressor compresses" are the same
sentence mechanically. Their correctness conditions are opposites:

- an **enable link** must be **null** — it makes a control live and changes
  nothing else. eq's null-test refuses one that moves the primary feature,
  because a link with side effects contaminates the arm that is not testing it.
- an **excitation step** must **not** be null — it exists to change the
  feature. Saturation's `Master XL -> 100%` moves THD by 100.41 dB, and that
  movement IS the verification.

**So eq's enable null-test would refuse a correct saturation plan.** The shared
type is right; applying one's verification to the other is not. Recorded
because the type invites exactly that mistake, and the next suite to declare a
plan will have both mechanisms in front of it.

---

## THE FLOOR-UNIT GUARD PROTECTS THE DIMENSION, NOT THE QUANTITY (3 August 2026)

Recorded beside the guard itself in `EjmapProbeRoute.h`, because a reader of
the guard will otherwise assume it covers more than it does.

`Probe::Floor` pairs a floor with its unit and `emitVerdict` refuses a
mismatch, which closes the hazard that a dB floor silently turns an octave
verdict from `contradicts` into `inconclusive`. It does **not** close this:

**Instance 1.** Saturation's verdict used `InstrumentFloor::depthDb` = 0.088 --
eq's spectral **lobe-depth** floor -- as the floor for a **THD** measurement.
Both are decibels. The guard was satisfied and said nothing, while the number
described the repeatability of a different measurement, of a different feature,
on a different subject.

A floor for the wrong quantity in the right unit is invisible to a unit check.
The fix was to the suite, not the guard: measure sigma_THD on this feature, on
this subject. **Nothing currently answers "is this floor a floor for THIS
measurement"**, and the guard should not be read as if it did.

---

## A NOISE FLOOR CANNOT CERTIFY AN EFFECT (3 August 2026)

**Instance 1: saturation's excitation verification.** It verified the plan by
`excMoved > 4*sigma_THD`. On a deterministic plugin sigma_THD is 0.0000, so the
bar was 0.004 dB — and a constructed plan naming the wrong control moved THD
0.15 dB, cleared it, and was reported **VERIFIED** while the drive walk that
followed moved 0.00 dB. The suite then told the operator *"the suppressor is
not the plan"* about a plan that was the entire problem.

4σ answers "is this distinguishable from noise". Verification asks "did this
establish the state the measurement needs", which is a different and much
larger question. The other suites use an absolute 1 dB for exactly this claim
(comp's `curveDelta > 1.0`, eq's `excF.maxAbsDb > 1.0`); saturation had
inherited the noise floor by accident.

Now `max(1 dB, 4σ)`, and the carve-out 1 message **names the plan itself as the
first suspect when the plan is unverified** rather than exonerating it.

---

## PROSE ASSERTING WHAT A MEASUREMENT SHOWED, WITH NOTHING MEASURING IT (3 August 2026)

The false-comment class one layer in, and in the worst possible place: inside a
suite whose entire purpose is verification.

**Instance 1.** Arm B printed `-- probing freq_hz, the other two held` on every
isolation block. Nothing held anything. The reference was re-established before
each block, so a parameter that drifted *during* a block would have been
silently corrected at the start of the next one and never seen. The sentence
described an intent, and a reader — including the person writing the next
check — would take it for a result the suite had established.

It is now `B-hold`, an assertion: after each block the two parameters not being
probed must still be at the reference, drift < 0.005. Three new assertions,
reading 0.00000 on AMEK. The sentence became a check, and the check passes; the
point is that until it existed, nobody could have known either way.

**Why this is worse than an ordinary false comment.** An out-of-date comment
misleads a reader of the source. This misled a reader of the *output* — the
artefact the tool exists to produce, in a suite that publishes verdicts about
other people's correctness. A verification tool asserting an unverified
premise in its own report is the failure it exists to catch, pointed inward.

**Audit of the other suites (3 August 2026).** comp's three section headers
carry the same shape more weakly: `THRESHOLD (excitation: ratio at max,
verified)`, `RATIO (excitation: threshold at -30 dB, verified)` and `ENVELOPE
(excitation: threshold -30, ratio max, verified)`. Each section does WRITE the
state it names, immediately, so the claim is not false — but "verified" refers
to P4's one-time verification by signal, not to a re-measurement at that point,
and nothing checks the state survived the writes between sections. comp has no
equivalent of `B-hold`. Recorded as a gap in comp rather than fixed here:
eq's session should not quietly edit another suite's assertions.

---

## A NEW CHECK MUST NOT SILENTLY THRESHOLD A QUANTITY AN EXISTING CHECK DELIBERATELY LEFT UNTHRESHOLDED (3 August 2026)

**Instance 1.** Item 3's role verification first required the mid band to move
less than 4σ_depth while the claimed width control was written. It **failed
the signed AMEK fixture**: engaging Mono Maker moves the mid band 0.437 dB
against a 0.352 dB floor.

That number is neither a defect nor a discovery. Arm A already records it,
with its cause, as *"recorded, not a criterion: M=(L+R)/2, so mono-ing below
the crossover necessarily moves side content into mid. Expected physics."* A
previous session measured it, explained it, and decided on purpose not to give
it a threshold.

The new check assigned one anyway, and the first thing it disqualified was the
fixture the settlement came from.

**Same family as feeding correct machinery an input somebody else already
decided about.** The check was not wrong in construction — a width control
that moves the mid band IS suspicious in general. It was wrong because the
quantity had already been adjudicated, and the new code re-opened the question
without knowing it had been closed. The machinery works; the input was
somebody else's settled decision.

**The rule:** before thresholding a quantity, look for whether an existing
check already measures it. If one does and assigns no threshold, the reason is
in the code beside it, and a new threshold must argue against that reason
explicitly rather than silently outvote it.

The corrected criterion is **dominance**, not absolute: mid must be a minority
(≤ 0.25×, the project's declared constant) of the side movement, which is what
actually separates a width control from a gain. The absolute mid figure is now
reported the same way arm A reports it — recorded, not a criterion.

---

## A GUARD IS ONLY FALSIFIABLE WHERE IT MATTERS (3 August 2026)

**Instance 1: the enable null-test.** A wrong enable link is **invisible when
the control is already live in the instance's default state**. Measured: a link
pointed at `V-Gain` instead of `Mono Maker In` passed the gate, correctly —
AMEK's Mono Maker is engaged by default, so the link makes no difference and
nothing can tell whether it was right.

The test catches links that **do damage** (the global `Power` specimen moved
the primary feature 4.391 dB and was refused), not links that **do nothing**.

This is the second known limit on M9's role machinery, beside the first: role
verification by signal cannot separate the claimed width control from any
other width control, because two controls with the same measurable character
are indistinguishable by that character.

**Both limits now print at the point of use**, not only here. A limit recorded
only in a proposal is invisible to the person reading a passing run, and a
passing run is exactly where a limit needs stating — it is the moment someone
concludes more than the evidence supports.

---

## ATTRIBUTION IS LOST WHEREVER EVIDENCE FROM TWO SOURCES IS MERGED BEFORE IT IS ATTRIBUTED (3 August 2026)

**Instance 1.** comp's first map-claim implementation kept ONE report for the
whole suite. A specimen with a correct threshold ladder and a corrupted ratio
ladder — every threshold point exact to 0.00 dB — reported
`CONTRADICTS threshold_db`. The measurement was right, the merge destroyed the
attribution, and the sentence named a parameter that was fine.

The fix is structural, not careful wording: **one report per semantic, each
gating its own verdict, in its own units.** Merging is what loses the
attribution, so nothing is merged.

**Where it bites next: eq.** Three parameters share a band — `freq_hz`,
`gain_db`, `q` — driven against one spectrum and one set of lobe features. A q
failure reported as `freq_hz` is this defect exactly, and the shared feature
extraction makes the merge the natural way to write it. Item 3 carries the
same rule in: per-semantic reports, per-semantic gates, per-semantic units
(octaves, dB, and a width ratio — three different units on one band).

This is a sibling of *routing right with wrong words*: there the route was
correct and the language wrong; here the measurement is correct and the
subject wrong. Both produce a sentence that is confidently about the wrong
thing.

**THE FIX IS NOT THE SAME IN BOTH SUITES, AND THE DIFFERENCE MATTERS.**

In comp the pooling was a **reporting** choice: threshold and ratio are
measured by separate walks, and one shared report merged two independent
results. Splitting the report fixed it, and no measurement changed.

In eq the pooling is **physical**. `lobeFeatures` returns centre, depth and
width from ONE pair of Welch spectra, and the three quantities are coupled: a
wrong q shifts the measured centre and depth, a wrong gain changes depth and
through lobe fitting the apparent width. There is no separate result to
un-merge. Attribution there requires **isolation probes in the measurement** —
move one semantic at a time from a common reference state and attribute each
feature to the parameter that actually moved — which restructures arm B rather
than reorganising its output.

Recorded explicitly because someone reading comp's fix will otherwise apply it
to eq, split the report, and get three separately-labelled numbers that are
still derived from one coupled measurement. That is worse than the pooled
version: it looks attributed and is not.

---

## ANY FIXED EXPLANATORY TEXT AT A SHARED EXIT IS THE SAME TRAP (3 August 2026)

A shared exit knows the shape of what it is reporting and nothing about the
caller's subject matter. Text written there answers for callers it knows
nothing about.

- **`emitInconclusive`'s basis, twice.** A fixed suffix said *no measurement
  was taken*, false at the excitation guard where four renders were taken.
  Replacing it with a bool produced a second fixed suffix saying *the feature
  never appeared*, false at the envelope sites where tau moved 11.5 → 63.7 ms
  and the obstacle was an undeclared unit family. Two false suffixes in three
  lines of the same function.
- **`routeText`'s units.** Found by the audit this class prompted. Every route
  sentence hard-coded **dB**, so comp's ratio verdict — a slope delta, output
  dB per input dB — printed `moved 0.38 dB against 0.40 dB predicted` about a
  ratio, and attack/release printed a log2 time ratio as decibels. `unit` is
  now a REQUIRED argument at both `routeText` and `emitVerdict`: five call
  sites, five different answers (dB, dB, dB, `dB/dB`, `log2 ratio`). Making it
  required rather than defaulted is deliberate — a default is how the trap
  regrows.

**THE RULE: a shared exit may format, count and route; it may not explain.**

Explanation belongs to the caller, who is the only one who knows why.

The rule covers **both materials the defect appeared in**, which were the same
defect wearing different clothes:

- **Fixed explanatory text** — `emitInconclusive`'s basis, asserting a cause
  ("no measurement was taken", then "the feature never appeared") that was
  false for callers the exit knew nothing about.
- **A hardcoded unit** — `routeText`'s "dB", asserting a dimension for
  measurements that were slope deltas and log2 time ratios.

A unit is an explanation: it says what kind of quantity this is. Writing one
at a shared exit is the same act as writing a cause there, and it fails the
same way. Both are now caller-supplied and both are **required**, not
defaulted, because a default is how the trap regrows.

**Audit result (3 August 2026):** `routeText` was the last shared exit
carrying fixed explanatory text. `emitVerdict` now composes only from
caller-supplied units and evidence; `emitContradicts` and `emitInconclusive`
take their whole reason and basis from the caller; `assertHarness` prints
numbers only; `MapClaimReport` states what it measured without saying why.

---

## DOCUMENTATION ASSERTING A PROPERTY THE CODE LACKS (new mechanism, 3 August 2026)

A member of the verification family, and the worst-behaved one: it does not
fail to answer the reader's question, it answers it confidently and wrongly.
Absent documentation makes a reader go and look. False documentation stops
them looking.

**Instance 1.** The limiter/gate suite carried, directly above its target
selection: *"The qualified target: from the map when one exists, else the
suite's declared preference."* No map is read anywhere in that branch, and
none ever was. A reader — including the one who wrote the parameterisation
proposal — concludes the suite is already half-parameterised and that only the
fallback needs work. The truth is that the name list IS the mechanism.

Two console lines carried the same implication to the operator: *"A map
qualifier is required"* and *"the map must qualify which member of the pair is
meant"*, printed by a suite that cannot consult a map. Both now say plainly
that the qualifier would resolve it and that this suite cannot read one yet.

**Why it is distinct from the misplaced guard.** The misplaced guard is code
that is present but unreached: running the path exposes it. This one has
nothing to run. It survives every test, because it is not executable. Only
reading the code *against* its comment finds it, and the comment is precisely
what persuades a reader they need not.

**The check that finds it.** For any comment asserting a data source, a guard
or an invariant, name the line that implements it. If you cannot, the comment
is a claim under test and not a description. Applied across the suites this
session, the other data-source claims held — the gate's burst gap really is
read from the plugin's own release ladder (`MainComponent.h:2074`), and
saturation's pinned index really is name-guarded before use.

---

## VERIFICATION MUST READ WHAT THE CONSUMER READS

**The general rule, stated once above the instances: confirming a write by
the writer's return value tests the writer, not the artefact.** Four
instances in this module, the same shape in four different materials:

| the write | what was checked | what the consumer reads |
|---|---|---|
| `writeConfirm` on a parameter | `getValue()` — the property plane, which answers instantly | the DSP, which needed the runloop serviced. Five sessions of misdiagnosis |
| the probe stake's plugin id | the string, by eye, and it looked plausible | `ScannedPlugin::pluginId()`. A doubled prefix would have keyed the quarantine row to a plugin that does not exist — downstream that reads as NO row, not a wrong one |
| proposal edit (backticked identifiers) | the edit returned | the file, where the shell had eaten three identifiers before python saw them |
| proposal edit (stale anchor) | the edit returned | the file, unchanged — the anchor no longer matched |
| proposal edit (invented anchor) | the edit returned | the file, unchanged — the anchor **never existed**; `grep` for it returns nothing |

Three of those five were caught by chance, one by a later run, and only the
last by an actual check. **Verification has to read the thing the consumer
reads** — the rendered signal, not the property; the id scheme's own
constructor, not the eye; the file on disk, not the writer's return value.

A corollary worth keeping: a check that cannot fail is not a check. `grep -c`
on text you also wrote in the same breath tests nothing unless the pattern
comes from the consumer's side of the contract.

## THE MISPLACED GUARD — a named defect class, beside the silent-drop list

Three instances in this module, all the same shape, all fixed the same way.

| instance | the guard | where it was | where it belonged |
|---|---|---|---|
| compressor envelopes issued verdicts without consulting the mode guard | `displayIsModeToken` | existed, in the harness | called before every envelope verdict |
| gate wrote over-claim text on its own authority | the over-claim/deafness fork | existed, in the gate suite | `routeVerdict` in the shared path |
| probe stake applied per-site | `ledger.beginLoad` | present at 5 of 16 `host.load` sites | inside `host.load` itself |

**The signature, and it is what makes this class expensive:** the guard is
not missing. Reading the code shows it present, correct, and recently
worked on. **Only running the path shows it absent.** Every one of these
three was found by executing a case, never by inspection — and in two of
the three the guard had been added *in the session immediately before*, at
the sites then in view.

**The fix is never another call site.** Adding a sixth `beginLoad` would
leave the seventh site to be forgotten by whoever writes it next, which is
precisely how the first five accumulated. The fix is to move the guard to
the one place every path already goes through, so forgetting becomes
impossible rather than merely unlikely. The same reasoning produced
`duplicateIndexConflicts` in the shared header (after the rule existed
twice and the copies drifted) and `routeVerdict` in the probe header.

**A fourth instance, and it is the tool blaming a plugin for its own
defect.** Not a new class — the same shape, caught the same way:

| the tool said | the truth |
|---|---|
| Mono Maker is a band frequency | the classifier read a name |
| the limiter's ceiling is 3.01 dB off | an RMS plateau read against a peak ladder |
| the ceiling error grows to 4.82 dB | a five-step mean contaminated by unlimited steps |
| kHs Gate failed to load | it loads fine — but NOT for the reason recorded here. See the correction below: the "wrong catalogue" reading was itself wrong |

Every one was caught by something **recording what happened** rather than
what the code believed — a probe measuring effect, a convention declared
next to a number, a per-ceiling gain-reduction line, a ledger detail string
at the choke point. None was caught by reading the code, and in each case
the tool's own report was the most confident-sounding thing in the room.

**A fifth instance, with the sharpened signature.** Checking a written
artefact BY EYE tests it against your belief about the format; only
comparing it against its CONSUMER tests it against the format. The stake's
doubled `AudioUnit:AudioUnit:` prefix read as plausible on screen and would
have produced a quarantine row keyed to a plugin that does not exist —
which downstream reads as **no row at all**, not as a wrong one. Same shape
as `writeConfirm` reading the plane that confirms instantly: both checked
the thing that answers, not the thing that matters. The rule that follows:
an identifier written for another component to key on is constructed by
**that component's own constructor**, never assembled at the write site.
The first correction to this bug was itself a second invented scheme
(conditional prefixing, still wrong for VST3, whose `fileOrIdentifier`
carries no format prefix to detect) — which is the argument for the rule
rather than for more careful assembly.

**The family has TWO distinct fixes, and the second is harder to see.**
Move the guard to the choke point every path already goes through — or,
when a path *never reaches* a choke point, give that path its own
recording. The second is harder because the guard looks correct where it
sits and the path looks unrelated to it: nothing inside `PluginHost::load`
can record a name-lookup miss, because a miss never calls it.

**CORRECTION (2026-08-02). kHs Gate was recorded here as the specimen and
is not one.** It resolves in the AU census (`[3] Threshold`), loads, and
renders 234 dB across its threshold ladder. The harness *found* it;
`host.load` failed at that moment for reasons still unknown, and the
"searched the wrong catalogue" reading was inferred from a VST3 ledger row
that had come from the SCAN path. The recorder (`resolveSubjectByName`) is
built and correct and **has no demonstrated instance** — two attempts to
construct one (TDR SlickEQ M, MTurboReverb) both resolved in the AU census
too. Kept by name rather than deleted, because the failure mode is the
point.

**A sixth instance of the family, and the cheapest to make.** That claim
entered this document and two rounds of instructions on a plausible
reading, unchallenged, until something ran against it. Nobody measured it;
it sounded right and was repeated — including by the reviewer, who wrote
"kHs Gate is the specimen" into the next instructions on the strength of
my report. **The family's signature applies to prose as well as code:** a
claim that reads as plausible is tested against belief, and only executing
it tests it against the world. This one cost two sessions of building the
right mechanism against the wrong motivating example.

**A seventh instance, and it is the module's most-filed defect stated as a
rule: A NAME IS NOT EVIDENCE OF BEHAVIOUR.** Three times, and each time the
name was *correct* — it was the inference from it that was wrong:

| the name | what was assumed | what was true |
|---|---|---|
| Mono Maker | a band frequency, mapped as `freq_hz` | a stereo-width control; the probe's headline contradicts |
| X-Gate `Lower Threshold` | the gate's open point | the hysteresis *close* point; the open point is a different index |
| bx_limiter `Release` | a live probe parameter for the sensitivity check | genuinely Release, and genuinely not expressible in a static level curve |

The third is the sharpest because it defeated a rule already written for
the first two: the ambiguity guard (enumerate candidates, decline to pick)
does not fire on an *unambiguous* match. The principle transfers where the
code does not — **suitability must be measured, never named** — which is
why the sensitivity check now tries several candidates and treats one
mover as proof and one null as nothing.

**An eighth entry, and it is a procedure rather than an instance: A DEFECT
FOUND IN ONE SUITE MUST BE CHECKED IN THE OTHERS BEFORE CONCLUDING THEY
ARE UNAFFECTED.** The Delta_pred defect — predicting against a full ladder
span while the feature moved only across the probed points — was found in
the limiter. The gate carried it **identically**, and was invisible because
a null feature absorbed the symptom: with nothing moving, no comparison
could look wrong. Fixing it un-masked a gate threshold that tracks (52.00
dB measured against 54.00 predicted), the first real verdict that suite
ever produced.

**Masking is what made it hard to see, not the defect.** A suite that is
already failing for another reason cannot report a second defect, so the
quiet ones are where shared inputs go wrong unnoticed. The exposure check —
grep the input, not the symptom — is what found it, and it costs one search
against the alternative of rediscovering the same bug per suite over
following sessions.

**Test the property, not the happy path.** A choke-point guard is only
proven by adding a caller that does *not* cooperate and confirming the
guard still fires — the forgettability property is the whole point, and a
passing happy path demonstrates nothing about it.

## THE SUITES ARE FIXTURE-BOUND, WHICH BOUNDS THE BATCH RUNNER (measured 2026-08-03)

Found by building `--probe-batch` and running it, not by reading the suites.

Each suite resolves ONE product by id and measures it: eq `aumf,ameq,Brwx`,
comp `aufx,APCM,ksWV`, limiter `aufx,bxa2,Brwx`, gate `SSL X-Gate` by name.
The eq suite goes further and hard-codes the signed AMEK fixture's shape --
`group1`, and Mono Maker at indices 7/8.

So `gateM9("comp")` called while iterating an arbitrary compressor's map loads
API-2500, measures API-2500, and files API-2500's verdicts under that map's
fingerprint -- then POSTs them there. The map is never touched. Nothing in the
suite would report anything wrong, because nothing in the suite is wrong; the
falsehood is entirely in the attribution.

**The runner therefore probes a map only when the map's own identity IS the
suite's signed subject**, and counts every other map as uncovered with the
reason named in the report. Proven by attempting it: a compressor-labelled map
carrying AMEK's identity is refused with "the comp suite is bound to API-2500
(m) and would measure THAT plugin", 0 probed, 0 posted.

**Consequence for the campaign:** the runner is an unattended re-runner for the
four signed fixtures today, not a corpus prober. Probing a real corpus needs the
suites parameterised by subject -- subject in, map's own indices in, fixture
constants out. That is a milestone-sized piece of work and it is the thing
standing between M9 and the corpus, so it belongs at the top of the resume list
rather than inside the runner.

**Second finding from the same run:** the runner first reported AMEK as "0
decided, 3 not covered". Both numbers were false. The eq gate publishes its
three parameter verdicts through `assertHarness` (B6), not `emitVerdict`, so the
row capture saw none of them; and the per-parameter enumeration walked `params`
only, so 5 groups and 62 controls were absent from a report that read as
complete. Corrected, the same run reads **3 decided, 77 of 80 mappable slots not
covered** -- which is the honest coverage headline and a far more useful one
than "probed 1". A report that enumerates a subset of the surface understates
the uncovered area silently, which is the silent-drop class wearing a coverage
table.

---

## Open questions (all prior ones decided or answered by measurement)

0. **The frequency gate vs its own σ floor — raised, not resolved, because it
   touches a signed number.** The proposal's rule is "every row is
   max(stated tolerance, 4σ_f)". With σ_centre floored at the measured
   instrument resolution (0.0322 oct ≤ 250 Hz), 4σ_centre = **0.129 oct**,
   which is nearly double the signed 0.070 oct (5%) frequency gate. Applying
   the max rule would silently loosen a gate the plan signed. The build does
   **not** do that: the confirm tolerance stays 0.070 oct, and the instrument
   floor enters only the expressibility test, on the reasoning that the
   instrument error is a *systematic* per-frequency bias (repeatable, signed,
   as the table shows) rather than random noise — and multiplying a
   systematic by 4 is not a statistical argument. Decision needed: accept
   that reasoning as the standing rule for systematic floors, or re-cut the
   frequency gate with the instrument floor included?

1. **DPR-402's two pair-gated frequencies**: the census's single-flip probe
   could not wake them (depth-2 enable combinations). Accept
   `suppressors untested at depth 2` as the census's honest limit, or fund a
   depth-2 flip pass on that one product before build?
2. **Sidechain-variant products** (comp-sc class): the census suggests the
   server's product metadata should mark them, since any dial without a fed
   sidechain is inert. Server-side note, or ejmap evidence field?
