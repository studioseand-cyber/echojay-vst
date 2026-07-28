# Surgical EQ — Handoff / Status Brief

**Branch:** `feat/surgical-eq` (off `feat/session-b-state`). **Read `SURGICAL_EQ_DESIGN.md` first** for the full architecture and rationale; this file is the live status + next-actions brief.

Goal: a built-in surgical EQ that EchoJay *owns*, so AI-suggested surgical moves apply with exact accuracy (per band) instead of going through the lossy anchor-table path used to drive third-party plugins. It must appear in the chain like a hosted plugin (a "slot"/"link") and have its own editable UI. Plan is the **hybrid → Route B** from the design doc: build the engine/UI/apply layer route-agnostically (all new files), then integrate into `ChainHost` as a built-in node **last**, rebased onto latest `feat/session-b-state` to merge clean.

## Why this matters (the core insight)
The existing AI apply path (`EchoJayParamApply.h` + `EchoJayParamMaps.h`) drives third-party plugins by reverse-mapping a value against an **offline-built anchor table** (`maps/<fingerprint>.json` from the `ejextract` harness), interpolating to a normalized param write, then reading back and reverting if off. That is inherently imprecise: grid-quantized, no per-band addressing, EQ params aren't in the "CORE" dialable set, and it needs a matching map to exist at all. An EQ EchoJay owns bypasses all of it — a move becomes a direct typed `setBands` write. That bypass is the whole point of this feature and is already implemented at the engine + apply-logic level.

## What's DONE and VERIFIED (numerically, in-sandbox with g++)
All new files; **no existing EchoJay file was modified** except a 6-line addition to `CMakeLists.txt` (main target sources).

- **`Source/EqEngine.{h,cpp}`** — headless, JUCE-free DSP core. TPT/SVF (Cytomic) filters: bell, low/high shelf, notch, variable-slope HP/LP (Butterworth Q-staggered cascade, 12–96 dB/oct). Thread-safe param publish via seqlock (message→audio), block-rate log/dB smoothing, per-band solo, global bypass. **Dynamic (threshold-driven) bands** for de-ess/resonance taming (detector bandpass + stereo-linked env follower + range clamp; Bell only). Exact analytic magnitude response (state-space) so the UI curve matches the audio path.
- **`Source/EqMove.h`** — JUCE-free translation of an AI `eq_bands` move into exact `BandSpec`s. Per-band **merge** semantics (never clobbers unrelated bands): explicit 1-based `band` targets exactly; `band<=0` auto-allocates lowest free band; `disable` turns a band off. Tolerant type parsing. Overflow reported, not dropped.
- **`Source/SurgicalEqProcessor.{h,cpp}`** — `juce::AudioProcessor` wrapper. Stereo I/O, drives `EqEngine`. **No APVTS** (matches EchoJay house style); hand-rolled JSON state. `applyEqBands(var)` = direct exact apply; `currentEqBandsVar()` serialises state back. **Placeholder editor only** so far.
- **Tests:** `test/eq_engine_test.cpp` (analytic-vs-measured across all band types, centre-gain accuracy, notch depth, HP/LP −3 dB + slope, shelf asymptotes, extreme-Q stability, multi-band summing, bypass/solo, dynamic engage/clamp) and `test/eq_move_test.cpp` (parsing, allocation, exact targeting, merge safety, disable, overflow, dynamic passthrough). Build/run:
  ```
  cd test
  g++ -std=c++17 -O2 -I../Source eq_engine_test.cpp ../Source/EqEngine.cpp -o eqtest && ./eqtest
  g++ -std=c++17 -O2 -I../Source eq_move_test.cpp -o movetest && ./movetest
  ```
  Both report ALL PASS.

## What is NOT yet verified
`SurgicalEqProcessor.{h,cpp}` and the placeholder editor were written **without a compiler** (the Cowork cloud sandbox has no JUCE/Xcode). They are written against EchoJay's linked JUCE and mirror `LinkProcessor` conventions, but the **first real compile-check has not happened**. That's the immediate next action.

## IMMEDIATE NEXT ACTION — compile-check
Toolchain on this Mac: Command Line Tools installed, JUCE 8.0.12 at `~/JUCE`, CMake installed. Build just the VST3 (fast; also builds JUCE the first time):
```
cd ~/echojay-vst
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target EchoJay_VST3 -j8
```
If it errors in any `Eq*` / `SurgicalEq*` file, fix in place (likely a JUCE `var`/`DynamicObject`/`MemoryOutputStream` API detail). Keep `EqEngine`/`EqMove` unit tests green after any change to those.

## ROADMAP after compile-check (in order)
1. **Real editor UI** — replace `PlaceholderEditor` in `SurgicalEqProcessor.cpp` with a curve editor: draggable band nodes (freq×gain), numeric freq/gain/Q entry, FFT analyzer overlay, per-band solo/bypass/dynamic toggles, dynamic-action metering (`EqEngine::getBandDynamicGainDb`). New Component, no host-UI changes needed — it auto-embeds via `createEditorForSlot` → `showInline` once hosted.
2. **ChainHost integration (Route B) — the only step that touches shared/in-flight code; do it LAST, rebased onto latest `feat/session-b-state`.**
   - Add a non-format load path so `ChainHost` can append a built-in `SurgicalEqProcessor` node without `formatManager_.createPluginInstanceAsync` (mirror the `SlotWetBlend` in-house-node pattern, `ChainHost.cpp:438`).
   - Register its name ("EchoJay EQ") in the recommendable/entries lists so `resolveByName` finds it and the AI can target it.
   - Extend the restore loader (`tryRestoreSlotsFromXml` → `restoreNextSlot`) with the synthetic-description branch (a built-in can't be recreated via the format manager).
   - Route apply: in `setSlotStructuredSettings`/`applyStructuredIfReady`, detect the built-in EQ and call `SurgicalEqProcessor::applyEqBands` directly — **bypassing `EchoJayParamApply` entirely** (exact). 
   - Add the Eq sources to the **`EchoJayLink`** target too (currently only in the `EchoJay` main target) so the EQ works in the Link insert.
3. **AI schema (server-side)** — extend the move contract so the model emits `eq_bands` (array of `{type, freq_hz, gain_db, q, band?, slope_db_oct?, dynamic?}`) when the target is the EchoJay EQ. Client parse/dispatch already exists (`applyEqBands`). Note: chain-**edit** ops currently don't carry `settings_structured` (`PluginEditor.cpp` ~14600) — decide whether retuning an already-placed EQ needs that path extended or a dedicated move.

## Conventions to follow (matched to EchoJay)
- **No APVTS** anywhere; state is hand-rolled JSON (see `PluginProcessor.cpp` get/setStateInformation, and `SurgicalEqProcessor`).
- Keep `EqEngine`/`EqMove` **JUCE-free and unit-tested**; JUCE lives only in `SurgicalEqProcessor` and the editor.
- Keep all ChainHost/CMake churn minimal and **last**, to avoid colliding with active `feat/session-b-state` work on the other Mac.
- `#include <JuceHeader.h>` is the include convention.

## Git
- Work on `feat/surgical-eq`; push to `origin`. Remote is `https://github.com/studioseand-cyber/echojay-vst.git`.
- Rebase onto latest `feat/session-b-state` before the integration step.
- End commit messages with the project's Co-Authored-By / session trailer as per prior commits.

## eq_bands move schema (what `applyEqBands` accepts)
```json
{"eq_bands":[
  {"type":"bell","freq_hz":203,"gain_db":-3.0,"q":4.5,"band":3},
  {"type":"notch","freq_hz":6100,"q":8.0},
  {"type":"highpass","freq_hz":80,"slope_db_oct":24},
  {"type":"bell","freq_hz":6500,"gain_db":0,"q":5,
   "dynamic":{"threshold_db":-20,"range_db":-6,"attack_ms":2,"release_ms":60}}
]}
```
`band` optional (1-based; omit to auto-allocate). `disable:true` turns a band off. `enabled` optional (defaults true on a set).
