# ejmap — Plugin Mapping Tool Build Spec

**Date:** 29 Jul 2026
**Status:** Spec for review, nothing built
**Repo:** `echojay-vst-v200`, new CMake target alongside `tools/ejextract`

---

## 1. Why this exists

Two days of work established where the parameter-mapping pipeline succeeds and
where it fails:

| Stage | State |
|---|---|
| Enumeration (which plugins exist) | **Solved.** 92.9% product-resolvable via AU registry walk |
| Anchors (normalised → real value curve) | **Solved.** Set-then-read fixed the `getText` liars |
| Delivery (map reaches the client) | **Solved.** AU/VST3 identity fix took it 9.5% → 92.9% |
| Map format | **Solved.** v2 products/layouts/aliases, groups, mode kind, suppression |
| **Semantics (what a parameter MEANS)** | **32%.** This is the entire remaining gap |

The AMEK EQ 200 incident is the canonical failure. The map resolved `freq_hz`
to index 7 (`Mono Maker`) and `gain_db` to index 3 (`V-Gain`), because AMEK has
15 frequency-class controls and 11 gain-class controls and the rule builder
took first-wins. EchoJay wrote 250 Hz to the mono-summing crossover and −2 dB
to a hidden attenuator, then reported *"Applied automatically"* with no caveat.
Read-back passed, because read-back verifies that a value landed at the mapped
index — never that the index means what the map claims.

Classification **infers** semantics from names and sweep text. A human clicking
the knob **observes** them. That is the difference between 32% bar clearance
and near-100% on any plugin a person actually maps.

**Goal:** a standalone tool that a human (Sean, then testers) uses to produce
maps that are correct by observation, uploaded with human-verified provenance,
covering the plugins users actually reach for.

---

## 2. Standalone, not in EchoJay

Decided. Reasons, in order of weight:

1. **Release coupling.** EchoJay's plugin release is blocked behind two
   unmerged branches and an unfinished Session B. A mapper inside EchoJay
   inherits all of it. Standalone ships this week and iterates daily.
2. **Iteration cost.** Every UI change to an in-EchoJay mapper costs a full
   build → sign → notarize → install cycle. Standalone is a rebuild.
3. **Tester friction.** "Install this app, load your plugins, click some knobs"
   gets twenty testers. "Install our plugin, sign in, enable dev mode, find the
   hidden panel" gets five.
4. **The UI doesn't fit.** Keyboard-driven batch, seconds per plugin, a queue
   you move through without dialogs. That is a dedicated tool's interface and
   it makes the Chain tab worse.
5. **`ejextract` is already 80% of it.** Standalone JUCE app, hosts plugins,
   enumerates the AU registry, sweeps parameters, set-then-read, worker
   isolation, ledger. The gap is a window and a gesture capture.

### 2.1 Shared code — non-negotiable

`ejmap` and EchoJay **must compile the same headers**. Two implementations of
map semantics would drift within a week, exactly as the dial predicate did
(`usableCoreCount` went stale and read `bx_digital V3` as not-dialable when it
was the best case in the store).

Shared, no forks:

- `Source/EchoJayParamApply.h` — anchor interpolation (bidirectional, segment),
  suppression handling, group/band matching, the display parser, read-back
  comparison
- `tools/ejextract/EchoJayParamExtractor.h` — parameter enumeration, sweeps,
  set-then-read, flat detection
- The v2 map schema and its sanitizer (pair-sort, trailing-junk truncation,
  mirror rejection, plateau rejection)

**Test that enforces it:** a map produced by `ejmap` must round-trip through
EchoJay's `applySettings` and produce the identical writes the mapper verified.
If the shared header changes, both binaries change together.

---

## 3. The core mechanism

### 3.1 Capture by polling, not by listener

The obvious approach is `AudioProcessorParameter::Listener` — subscribe, let
the user move a knob, read which index fired. **Do not rely on this alone.**
Earlier investigation found plugins that do not notify the host on GUI-driven
changes, which kills the listener path silently.

**Use polling as the primary mechanism:**

```
On "capture" arm:
  snapshot = [getValue(i) for i in 0..numParams]
  poll at 30 Hz:
    current = [getValue(i) for i in 0..numParams]
    diff = indices where |current[i] - snapshot[i]| > epsilon
    if exactly one index moved and moved > threshold:
      capture it
```

Polling works regardless of whether the plugin notifies. It is a few hundred
float reads at 30 Hz, negligible cost, and it cannot be defeated by a plugin
that keeps its host notifications to itself.

**Layer the listener on top** as a fast path and a disambiguator: when both
agree, capture instantly; when polling sees several params move at once (linked
controls, a macro), the gesture callbacks tell you which one the user actually
touched.

**Failure modes to handle:**

- **Nothing moves.** The plugin does not expose that control as an automatable
  parameter. Mark `not-automatable`, move on. This is a real and permanent
  result, not an error.
- **Several move together.** Linked stereo pairs, or a macro. Capture all of
  them as an `indices[]` set.

  **CORRECTED 2026-07-30.** This bullet used to end: "this is the same
  channel-twin structure the map format already supports and the same bug it
  fixed on MC 77 (only Attack L was written)". Both halves were false, and the
  claim was an inference from a parameter list rather than an observation.

  Measured: `indices` appears nowhere in `Source/EchoJayParamApply.h` or
  `EchoJayParamExtractor.h`, no commit ever added it there, and **0 of 4,233
  extracted maps** carry `indices` or any other multi-index field — every
  mapped parameter is a single `"index": N`. So the map format did not already
  support channel twins, and there was no mechanism by which a bug could have
  been fixed. No bug report, fix, test or map exists for it.

  What MC 77 does have is `Input/Output/Attack/Release/Ratio` in L and R
  variants plus `Link` and `IO-Link`, which reads exactly like the twins case.
  It is a dual-mono plugin: two channel strips, two Attack knobs, and moving
  one moves one parameter. Same shape as Waves API-2500 and Q1, both of which
  were checked against their GUIs and behaved the same way.

  **No confirmed channel-twin case has been found**, so `Kind::twins` is
  retired — see the risk register. `indices[]` stays in the schema because the
  `gesture` outcome populates it anyway and a real case may appear on a
  tester's machine. The shape that used to trigger the twins verdict (same
  direction, magnitudes within 1.5x) is still measured and recorded as
  `same_direction` and `magnitude_ratio`, as a description of what was seen
  and not as a claim about what caused it.
- **Continuous drift.** LFO-driven or metering params that change on their own.
  Take several baselines with nothing touched, build a noise mask of
  self-changing indices, exclude them from capture. Without this, any plugin
  with a moving meter produces garbage.

### 3.2 The classifier's verdict is the starting point

Do **not** present a blank form. Load the existing classification for that fp
(from `classified-v2/` or the served map) and present it as a proposal.

Most rows are already right. The human's job is *confirm or correct*, which is
several times faster than authoring, and it concentrates attention on exactly
the rows the model was unsure about (`unsure` and low-confidence verdicts sort
to the top).

This also produces the labelled dataset: every confirmation or correction is
ground truth against a model verdict, which is what you need to measure and
improve the classifier for the long tail nobody will ever hand-map.

---

## 4. The mapping flow

Per plugin, keyboard-driven, no dialogs.

```
1. LOAD          Tool instantiates the plugin, shows its native editor inline.
2. CATEGORY      Pre-filled from classification. Human confirms or picks from
                 a list. Category determines the dial set.
3. DIAL SET      Checklist for that category, pre-filled with the classifier's
                 proposed index per semantic.
                   eq        → freq, gain, Q (per band), in/out
                   comp      → threshold, ratio, attack, release, makeup
                   de-esser  → freq, sensitivity/range, mix
                   delay     → time or sync, feedback, mix
                   reverb    → decay, predelay, size, mix
                   sat       → drive, mix, tone, out
4. PER CONTROL   Tool highlights the target semantic ("Threshold").
                 Human wiggles the real knob on the plugin GUI.
                 Tool captures the index, shows the param name it captured.
                 SPACE confirms. N skips (not present on this plugin).
5. SWEEP         On capture, tool immediately sweeps that param (set-then-read,
                 21 points, same code as ejextract) and builds anchors.
6. VERIFY        Tool writes a test value through the SHARED apply path and
                 the human watches the knob land. SPACE confirms it moved to
                 the right place.
7. GROUPS        See §5.
8. SUBMIT        Map uploads with human-verified provenance. Queue advances.
```

**Speed target.** The sweep is the slow part: ~21 set-then-read cycles per
param, roughly 0.3–0.5 s. A 5-band EQ at 3 controls per band plus in/out is 17
params ≈ 8 s of sweeping plus 17 wiggle-and-confirm cycles. **Target 90 s per
multi-band EQ, 30 s per compressor.**

**Keyboard map:** `SPACE` confirm · `N` not present · `R` recapture · `←/→`
move through the checklist · `S` skip plugin · `⌘↵` submit and advance.

---

## 5. Groups fall out of the flow

This is the part that fixes AMEK, and it needs no inference at all.

After the human maps band 1 (freq, gain, Q), the tool has three indices and
their parameter names. It then:

1. **Pattern-infers the remaining bands.** `LF Gain 1` → `LMF Gain 1` →
   `MF Gain 1` is an obvious progression, as is `band1 freq` → `band2 freq`.
   Present the inferred bands as a proposal.
2. **Human confirms in bulk.** One keypress accepts all inferred bands; any
   band can be corrected by wiggling.
3. **Ranges come free.** Each band's freq anchors give its reachable range,
   which is what the client band matcher uses to pick a band that can reach the
   requested frequency.

Note from the AMEK analysis: `LF Freq` and `LMF Freq` both read 15–780 Hz, so
range does **not** disambiguate bands. **Group membership does the work**;
range is a tiebreaker that is allowed to fall through to first-free. The
critical property is that `Mono Maker` is *not in the group*, so it can never
be selected regardless of range.

Also capture:

- **Family tag** when a plugin has more than one band family (E2Deesser has
  `sband` and `vband` families doing different jobs; a flat band list
  mis-merges them)
- ~~**Channel twins** as `indices[]` when L/R or M/S duplicates exist~~
  **Retired 2026-07-30, no confirmed case.** L/R duplicate *parameters* are
  common (18.2% of maps) but say nothing about how many *controls* the GUI has,
  and every candidate checked turned out to expose one knob per parameter.
  `indices[]` is still populated by the `gesture` outcome. See the corrected
  "Several move together" bullet above and the risk register.

---

## 6. Anchors, and the text-liar case

Anchors come from the existing set-then-read sweep. Most plugins are fine.

**Text-liars** (Valhalla class: `getText(normalizedValue)` ignores the argument
and returns current state) need a human in the loop, and this is where a
standalone tool with a person sitting at it beats anything automated:

```
Tool sets the parameter to n = 0.0, 0.25, 0.5, 0.75, 1.0
For each: "What does the GUI show?" → human types the value
Tool builds anchors from the typed values
```

Five typed values per param is slow, so only do it for params the flat-sweep
detector flags as text-liars. On a Valhalla reverb that is every param, which
is a genuine cost — but it is the only way those plugins become dialable at
all, and it needs doing exactly once ever, per plugin version.

**Sanitize on capture, not later.** Run the same sanitizer EchoJay's builder
uses (pair-sort descending, truncate trailing junk, reject mirrors, reject
plateaus) so a hand-mapped anchor set cannot be worse than a generated one.
Show the human the resulting curve and let them reject it.

---

## 7. The vocabulary ceiling

**This is the constraint that limits "every parameter perfectly", and it needs
a decision.**

A parameter can be captured perfectly and still be undialable, because the
semantic vocabulary has no kind for it. `spiff`'s `sensitivity`, `sharpness`,
`cut depth` and `boost depth` are the archetype: clear meaning, no schema slot.
Mapping them by hand changes nothing unless the vocabulary, the validator and
the chain prompt all know the kind exists.

**Proposal: two tiers.**

**Tier 1 — typed semantics.** The existing vocabulary (`threshold_db`,
`freq_hz`, `mix_pct`, `feedback_pct`, `mode`, …). These drive category bars,
`settings_structured`, and the dialable flag. Closed set, validated, known to
the prompt.

**Tier 2 — named controls.** Any parameter the human maps, carrying its real
name and its anchors, with no typed kind:

```json
"controls": {
  "sharpness":  { "index": 4, "range": [0, 10], "unit": null },
  "cut depth":  { "index": 1, "range": [0, 10], "unit": null }
}
```

The AI is told these exist, and can emit `"sharpness": 6`. The map resolves it
by name, the anchors convert to normalised, the write lands. No new kind, no
validator entry, no category-bar change.

**Why this matters:** Tier 2 is what makes "dial every parameter perfectly"
actually achievable. Tier 1 will always be a closed set that lags real plugins;
Tier 2 has no ceiling. Tier 1 still governs *dialability* (category bars,
strict mode) because those need typed semantics to reason about roles.

**Decide before building:** does the chain prompt get a per-plugin control list
when a Tier 2 map exists? That is a token cost on every chain turn for that
plugin, and it needs a contract rule ("only name controls in this list").

---

## 8. Verification

Three layers, all inside the tool:

1. **Write-back through the shared apply path.** Not a test harness — the
   actual `applySettings` from `EchoJayParamApply.h`. If it verifies in the
   mapper it is guaranteed to apply in EchoJay.
2. **Read-back comparison.** Same typed comparison as the runtime net: numeric
   with tolerance and unit carried through for continuous, label set and order
   for discrete. Catches an anchor that interpolates wrongly.
3. **Human visual confirmation.** The one thing read-back structurally cannot
   do: confirm the index *means* what the map claims. This is the whole point
   of the tool.

Record all three per parameter in the evidence payload.

---

## 9. Upload, provenance, trust

### 9.1 Trust enum

Extend the existing enum. Human-verified outranks everything:

```
rule-built  <  llm-classified  <  human-verified  <  admin-approved
```

Per **key**, not per map (the earlier evidence-design decision holds: 40 of 42
params verified is a usable map if you know which two are suspect). A map can
be human-verified on its dial set and llm-classified on the rest.

### 9.2 Payload

```json
{
  "fp": "…",
  "identity": { "format", "uid", "name", "vendor", "version", "param_count" },
  "category": "eq",
  "params": { "<semantic>": { "index" | "indices", "kind", "anchors",
                              "trust": "human-verified",
                              "method": "gettext" | "setread" | "human-typed" } },
  "groups": [ { "family", "n": <band NUMBER, one group object per band>,
                "primary": <true on the family the human touched>,
                "params": {…}, "freq_range": [lo, hi] } ],
  "controls": { "<name>": {…} },
  "evidence": {
    "captured_by": "poll" | "listener" | "both",
    "readback": { "<semantic>": { "wrote", "read", "match": true } },
    "visual_confirmed": [ "<semantic>", … ]
  },
  "provenance": {
    "tester_id", "machine_id", "ejmap_version", "extractor_version",
    "plugin_version", "host_os", "at"
  }
}
```

**Environment travels with the evidence, never with the identity.** That
distinction was established when version left the fingerprint key and it holds
here.

### 9.3 Server acceptance

- **Human-verified maps do not need consensus.** Consensus was always
  orthogonal to correctness — three contributors agreeing on deterministic
  classifier output is the same answer three times. One human observation
  outranks it.
- **But keep the corroboration counter.** Independent human maps of the same
  plugin from different machines *are* genuine independent evidence, and a
  disagreement between two testers is a signal worth surfacing.
- **Structural validation at the mouth:** map validates against the schema,
  anchors sanitize clean, no flat sweeps, indices in range, claimed category
  bar actually clears. Reject at the mouth, log the reason, never silently
  drop (this is the fourth instance of the silent-drop class in this codebase —
  `_db.js` whitelist, `validate-settings` unknown keys, `pointScale` bare-k,
  the unextractable undercount).

---

## 10. Queue and prioritisation

**Order by real suggestion frequency from analytics**, not alphabetically.
Suggestion frequency is power-law: the top ~200 plugins carry most user
exposure. Twenty testers mapping ten plugins each is a weekend and covers most
of what people actually load.

Queue construction per tester:

```
priority list (by suggestion frequency, descending)
  ∩ plugins installed on this machine
  ∩ not already human-verified in the store
  → ordered work queue
```

Second-order ordering within that: prefer plugins currently marked
`dialable: false` (biggest delta), and prefer categories with thin coverage
(de-esser is 8 dialable products; delay is 14).

**Show the tester their impact.** "You've mapped 12 plugins, 340 users have
these installed." Testers who see the number keep going.

---

## 11. Distribution and abuse

**Distribution:** standalone signed and notarized macOS app, small download.
Sign in with the EchoJay account (reuse `auth.json` if present, otherwise
browser OAuth). Server returns a tester flag; without it the app runs in
read-only mode and can map locally but not submit.

**Windows:** the extractor core is cross-platform but the AU registry walk is
macOS-only. Windows testers get VST3 only. Worth supporting in phase 3, not
phase 1.

**Anti-abuse:**

- **Allowlist.** Only invited tester accounts can submit human-verified maps.
- **Structural gate** as above.
- **Admin review queue.** New tester's first N submissions are held for review;
  after that they auto-accept. Cheap trust-building, no ongoing burden.
- **Disagreement flag.** If a submitted map contradicts an existing
  human-verified one, hold both and surface for review rather than last-wins.
- **Revocable.** A tester's submissions can be bulk-retracted by ID if
  something goes wrong.

---

## 12. Build phases

**Phase 1 — Sean-only, no upload.** Window with plugin editor, poll-based
capture, dial-set checklist, sweep on capture, write-back verify, local JSON
output. Proves the mechanism and the speed target. Map 20 plugins by hand and
measure the real per-plugin time.

**Phase 2 — groups and text-liars.** Band pattern inference and bulk confirm,
family tags, channel twins, human-typed anchors for text-liar params. Map AMEK
and Valhalla — the two plugins that broke in the two distinct ways.

**Phase 3 — upload and trust.** Payload, server acceptance, trust enum, per-key
provenance, structural gate at the mouth, admin review queue.

**Phase 4 — tester distribution.** Signed build, auth, allowlist, queue from
analytics, impact display.

**Phase 5 — Tier 2 named controls.** Schema, prompt contract, and the chain
turn changes. Only after Tier 1 is proven end to end.

---

## 13. Open questions for decision

1. **Tier 2 in or out of scope?** It is what makes "every parameter perfectly"
   real, and it is also a chain-prompt change with a token cost. Phase 5 above
   assumes in, later.
2. **Full pass or dial set only?** The spec assumes dial-set-first for speed.
   A "map everything" mode is available but slow — worth having for the top 20
   plugins only.
3. **What happens to existing llm-classified maps** when a human-verified one
   arrives for the same layout? Replace outright, or merge per-key keeping
   human where present? Recommend merge-per-key.
4. **Does the tool re-verify on plugin update?** A new plugin version changes
   the fp. Prompt the tester to re-map, or attempt name-based transfer of the
   human-verified map and re-verify only the anchors? Recommend the latter —
   names survived every version transition measured.
5. **Windows in phase 3 or later?**

---

## 14. What this does not fix

Stated plainly so the tool is not oversold:

- **The long tail.** 1,500+ plugins, testers will map hundreds. The rest stay
  on classified maps, which is why classifier quality still matters and why
  every human mapping should feed back as labelled training data.
- **Category bar definitions.** A perfectly mapped reverb with only decay and
  mix still fails a two-of-two bar. That is bar tuning, separate work.
- **The edit path.** Blocked on the `[AVAILABLE BUILTINS]` capability marker
  decoupling, unrelated to mapping.
- **Plugins with no automatable parameters.** If the control is not exposed,
  no amount of clicking captures it.
