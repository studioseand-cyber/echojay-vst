# EchoJay Surgical EQ — Design & Integration Options

**Branch context:** written against `feat/session-b-state` (v2.23.49), the active v2 branch. `main` is stale (v1.6.3) and has none of the chain/hosting machinery.

**Goal (from Sean):** a built-in surgical EQ that EchoJay *owns*, so that when the AI suggests a surgical move it can be applied with exact accuracy, per band. It must be insertable into the chain (as a slot / "link") and therefore usable from both the main plugin and the EchoJay Link insert. As surgical as possible. It also needs its own editable UI for manual tweaking.

---

## 1. Why third-party surgical moves are imprecise today

When the AI dials in a hosted plugin, the value does **not** travel as "−3 dB at 203 Hz." It goes through the offline auto-mapping system:

- `EchoJayParamApply.h` → `applyOne()` (`:329`) resolves a semantic key (`freq_hz`, `gain_db`, …) to a **hard parameter index** via a per-plugin map `maps/<fingerprint>.json`, where `fingerprint = SHA-256(format | uniqueId | version | paramCount)` (`EchoJayParamMaps.h:28`).
- The requested value is matched against an **anchor table** — `[[value, normalized], …]` pairs swept offline by the `ejextract` harness — and **piecewise-linearly interpolated** to a normalized 0..1 (`interpolateAnchors`, `:244`), then written with `setValueNotifyingHost` inside a change gesture (`:351`).
- The write is **read back** from the plugin's own display and **reverted** if it lands outside a tight tolerance (`typedReadbackMatch`, `:160`).

Four structural accuracy limits fall out of this, and none of them are bugs — they're inherent to driving parameters you don't own:

1. **Grid quantization.** The value lands wherever the nearest anchors interpolate to, bounded by anchor density — not on the exact target.
2. **No per-band addressing.** The semantic vocabulary is flat. There is no way to even *express* "band 3: freq + gain + Q as a group" to a generic hosted EQ.
3. **CORE-set bias.** The "dialable" gate counts compressor-shaped semantics (`ratio, threshold_db, attack_ms, release_ms, makeup_db, gain_db`, `:545`). EQ freq/Q aren't CORE, so many EQs never qualify.
4. **Map-must-exist.** No `maps/<fp>.json` for that exact fingerprint → `dialStatus = noMap` → **nothing is written** (`ChainHost.cpp:1641`), only prose.

**The fix is the whole point of this feature:** an EQ EchoJay owns has known parameters with known ranges, so a move becomes a *direct typed write* — exact, every time, per band. The anchor/interpolation/read-back apparatus is bypassed entirely for this one device.

---

## 2. What's shared no matter which route we pick

These pieces are identical for Route A and Route B. Roughly 80% of the work lives here and is **all new files** — zero conflict with your parallel work.

### 2a. The DSP core — `EqEngine` (headless)

A UI-free, host-free DSP class that does the maths. Keeping it headless is deliberate: two things drive the same engine — your hand on the UI and the AI's exact writes — without fighting, because both just push a `BandSpec` array into the engine.

- **Filter topology: TPT / state-variable (SVF), not Direct-Form biquads.** For surgical work this matters more than anything else — SVF stays numerically stable and holds its exact frequency/Q at high resonance and near Nyquist, where Direct-Form drifts and gets gritty. `juce_dsp` is already linked (`CMakeLists.txt:157`), but its stock `IIR` is Direct-Form; I'd hand-roll a Zölzer/Zavalishin SVF (~150 lines) for the bells/notches and use `juce::dsp` only for the analyzer FFT.
- **Band types:** bell, high/low shelf, true (infinite-depth) notch, and variable-slope HP/LP (6–96 dB/oct via cascaded SVF stages).
- **Per-band fields:** `type, freqHz, gainDb, q, enabled`, plus optional dynamic sub-record `{ thresholdDb, rangeDb, attackMs, releaseMs }`.
- **Dynamic EQ** (you said "as surgical as possible"): each band can be threshold-driven, so it only acts when the target frequency actually crosses a level — resonance-taming and de-essing without static loss.
- **Linear-phase mode** (per-band or global, FFT convolution) for cuts that can't be allowed to smear phase. Optional oversampling so high bells near Nyquist don't cramp.
- **Analyzer + band solo-listen:** so a proposed move is *verifiable* against what's actually there, and echojay (or you) can audition a single band before committing. A resonance-detection helper (scan the analyzer for peaks → propose notches) is a natural follow-on.

### 2b. The AI move schema — per-band EQ moves

Today `settings_structured` is a flat `{ "<semantic>": <value> }` bag (`EchoJayAPI.cpp:1653`, consumed at `PluginEditor.cpp:16880` → `ChainHost::setSlotStructuredSettings`, `:1474`). For a surgical EQ it needs a per-band shape, e.g.:

```json
{"eq_bands":[
  {"type":"bell","freq_hz":203,"gain_db":-3.0,"q":4.5},
  {"type":"notch","freq_hz":6100,"q":8.0},
  {"type":"bell","freq_hz":4200,"gain_db":-2.0,"q":1.4,
   "dynamic":{"threshold_db":-24,"range_db":-4,"attack_ms":5,"release_ms":80}}
]}
```

This means: (1) a small server-side prompt/contract change so the model emits `eq_bands` when the target is the EchoJay EQ, and (2) client-side parse + dispatch to the native setter. This is the same work in both routes; only the last mile (how the value reaches the EQ) differs.

> One gotcha to flag: chain-**edit** ops currently don't carry `settings_structured` (`PluginEditor.cpp:14600` "no settings_structured rides an op"). So "tweak the EQ that's already in slot 2" needs either that path extended or a dedicated move. Worth deciding early.

### 2c. The editor UI

A new `Component` — curve display with draggable band nodes, numeric entry for exact freq/gain/Q, the analyzer overlay, and per-band solo/bypass/dynamic toggles. Returned from the EQ's `createEditor()`, so it auto-embeds through the existing `createEditorForSlot` → `showInline` clip machinery (`PluginEditor.h:1372`) with no changes to the host UI, as long as it's sized to fit the clip rect (avoids the pop-out fallback).

---

## 3. The fork: how the EQ attaches to the chain

Everything above is common. This is the only real decision.

### Route A — Ship it as a separate "EchoJay EQ" AU/VST3

A small `juce_add_plugin(EchoJayEQ …)` target in the same CMake, containing `EqEngine` + editor. It's built into the installer and scanned like any other plugin.

**How it hosts:** as an ordinary slot. `resolveByName` / `namesMatchLoose` (`ChainHost.cpp:2090/2100`) finds it by name; `completeLoad` appends it as a normal graph node; `createEditorForSlot` shows its editor; state capture/restore work for free via its `getStateInformation`. **No `ChainHost` changes for hosting, UI, or state.**

**How exact apply works:** by default it would go through the anchor path like any hosted plugin — which is *not* exact. To make it exact you special-case it in the apply dispatch: detect the EQ by its uid/name in `applyStructuredIfReady` and route `eq_bands` to a direct typed setter (its own `AudioProcessorParameter`s set to real values, or a side-channel message) instead of `applyOne`. That's a contained bridge, not a rewrite.

| | |
|---|---|
| **Pros** | Lowest conflict with your parallel work — almost entirely new files + one CMake target + scanner registration. Editor, state, and both-surface support (main + Link) come for free. Clean separation. |
| **Cons** | It's a separately-installed plugin: it shows up in the user's scan list and must be shipped/registered. Exact apply still needs the special-case bridge. Slightly odd conceptually — a "built-in" that's technically an external scan target. |
| **`ChainHost` touch** | Minimal (one special-case in apply dispatch + name registration). |
| **Merge risk** | Low. |

### Route B — A true internal built-in node

The EQ is a `juce::AudioProcessor` compiled into **both** the `EchoJay` and `EchoJayLink` targets (exactly the pattern `SlotWetBlend` already uses — an in-house node in the graph, `ChainHost.cpp:438`), added directly as a graph node.

**How it hosts:** needs a new **non-format load path** in `ChainHost` — a synthetic `PluginDescription` and a `completeLoad`-style branch that appends a built-in node without `formatManager_.createPluginInstanceAsync`. Its name goes into the recommendable/entries lists so the AI can target it and `resolveByName` finds it. The **restore loader** (`tryRestoreSlotsFromXml` → `restoreNextSlot`, `:2498`) also needs the synthetic-description branch, since a built-in can't be re-created through the format manager.

**How exact apply works:** it's the *natural* path. `setSlotStructuredSettings` detects the built-in and calls a native typed setter with real values, per band. No anchor tables, no special-case — exact is the default.

| | |
|---|---|
| **Pros** | No separate install; always available. Exact per-band apply is the native path, not a bolt-on. "Part of the main plugin" is trivial — it can even be an always-present node. Best match for "as surgical / as accurate as possible." |
| **Cons** | Touches `ChainHost` in several hot spots: the load path, `resolveByName`/entries, the restore loader, and apply dispatch — consistently across both the main and Link targets. More new code in shared machinery. |
| **`ChainHost` touch** | Moderate, in the exact file you're actively editing on the other Mac. |
| **Merge risk** | Higher — `ChainHost.cpp` is central to your in-flight work. |

---

## 4. Recommendation

**End state: Route B.** It's the one that actually delivers "surgical and exact" as the default behaviour rather than as a special-case bridge, needs no separate install, and makes "always in the main plugin or any chain" trivial. It's the honest expression of "an EQ EchoJay owns."

**But sequence it to respect the parallel work.** The merge risk in Route B is entirely in the `ChainHost` touch-points, and those are a small fraction of the feature. So I'd build in this order on `feat/surgical-eq`:

1. `EqEngine` DSP + unit tests — all new files, zero conflict. (Section 2a)
2. The editor `Component` — all new files, zero conflict. (Section 2c)
3. The move-schema parse + native setter API on the EQ — mostly new. (Section 2b)
4. **Last:** the `ChainHost` integration (synthetic-description load path, restore branch, apply dispatch, name registration) — landed in one tight window, rebased onto your latest `ChainHost` so it merges clean.

That way 80% of the work accretes with no chance of colliding with your commits, and the 20% that does touch shared code lands deliberately and last, against your newest `ChainHost`.

**A hybrid is available if you want to defer the fork:** build steps 1–3 exactly as above (they're route-agnostic — a self-contained `AudioProcessor` EQ with a direct typed-apply API), and decide A vs B only at step 4 once your session-b work has settled. Starting the work does not require committing to the route today.

---

## 5. Open decisions before I write code

1. **Route A vs B** (or hybrid — start route-agnostic, decide at step 4). My rec: hybrid → B.
2. **Dynamic EQ in v1**, or static-only first with dynamic as a fast-follow? (Affects `EqEngine` scope but not its shape.)
3. **Linear-phase in v1** or later? (It's isolated — easy to add after.)
4. **Band count:** fixed (e.g. 8) or dynamic/add-as-needed? Surgical work usually wants add-as-needed.
5. **Chain-edit apply:** do we extend the edit-op path to carry `eq_bands` so the AI can retune an already-placed EQ, or is "set on insert" enough for v1?
6. **Analyzer/solo-listen in v1** or after the core cut/apply loop works?

Once you've picked a lane on #1 (and ideally #2–#6), I'll branch `feat/surgical-eq` off `feat/session-b-state` and start on the `EqEngine`.
