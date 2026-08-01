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
