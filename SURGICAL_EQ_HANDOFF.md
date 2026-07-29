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

## Editor (Step A) — DONE
`Source/SurgicalEqEditor.{h,cpp}` replaced the placeholder: log-frequency curve editor with draggable band nodes, wheel-for-Q, double-click add/remove, per-band strip (type, freq/gain/Q, slope, dynamic params, enable/solo), global bypass and a ±18/±24 view toggle. The curve is `EqEngine`'s own analytic response, so what is drawn is the audio path. The Step B analyzer overlay + dynamic metering are still outstanding; the "A" button exists but is inert.

## Chain integration (Route B) — DONE
Everything below is on `feat/surgical-eq`, rebased onto `feat/session-b-state`.

- **Built-in load path.** The EQ travels as a synthetic `juce::PluginDescription` with `pluginFormatName == "EchoJayBuiltin"` and a fixed uid. `loadPluginAsync` branches on it and constructs the node directly. That one branch is what makes the add menu, AI chain-edit ops, `loadByRecommendedName` **and session restore** all work — they already funnelled through there, so `tryRestoreSlotsFromXml` needed no change at all (the saved `CHAIN_SLOTS` XML round-trips the synthetic description like any other).
- **Menu.** `getFilteredPlugins` pins "EchoJay EQ" to the top, exempt from the AU/VST3 format filter (it is compiled in, so it is loadable either way) and badged "EJ".
- **Exact apply.** `applyStructuredIfReady` detects a built-in EQ slot and calls `applyEqBands` directly — no fingerprint, no map, no anchor interpolation, no read-back/revert. The branch sits *before* the fp/map gates, which would otherwise park the slot in "pending" forever since a built-in never gets a fingerprint. Accepts a bare `eq_bands` array or an object carrying one; reports honest applied/skipped counts (partial, not success, when the EQ is full).
- **Both targets.** The Eq sources build into `EchoJayLink` as well as `EchoJay`.

## What is NOT done
- **Server-side schema.** The model does not emit `eq_bands` yet — that contract lives in the backend (`/api/chat` at echojay.ai), not this repo. Until it does, the AI cannot dial the EQ by prompt. Everything on the client is ready to receive it.
- **Chain-edit ops carry no `settings_structured`** (`PluginEditor.cpp` ~14600), so "retune the EQ already in slot 2" still needs either that path extended or a dedicated move.
- **Step B**: analyzer overlay + dynamic metering.

## Dev affordance for testing exact apply now
Because the server does not emit `eq_bands` yet, `/eqtest` in the chat applies a hand-written JSON straight to the selected (or first) built-in EQ slot:
```
/eqtest {"eq_bands":[{"type":"bell","freq_hz":203,"gain_db":-3,"q":4.5,"band":3}]}
```
Gated on `ChainHost::devModeActive()` (the `/Users/SeanD/.echojay_dev` marker file), intercepted before the send-quota gate and before any network call. Release behaviour is unchanged.

## Build + install (what was actually run)
```
cd ~/echojay-vst
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target EchoJay_VST3 -j8     # fast compile-check
cmake --build build --target EchoJayLink  -j8     # Link shares the Eq sources
cmake --build build --target EchoJay_AU   -j8     # Logic is AU-only
cp -R "build/EchoJay_artefacts/Debug/AU/EchoJay V2.component" ~/Library/Audio/Plug-Ins/Components/
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"EchoJay V2.component"
```
Unit tests (must stay green):
```
cd test
g++ -std=c++17 -O2 -I../Source eq_engine_test.cpp   ../Source/EqEngine.cpp -o eqtest    && ./eqtest
g++ -std=c++17 -O2 -I../Source eq_move_test.cpp                            -o movetest  && ./movetest
g++ -std=c++17 -O2 -I../Source eq_post_tap_test.cpp ../Source/EqEngine.cpp -o posttest  && ./posttest
g++ -std=c++17 -O2 -I../Source eq_gain_test.cpp     ../Source/EqEngine.cpp -o gaintest  && ./gaintest
```

> **Duplicate-AU hazard.** There is a root-owned release install at `/Library/Audio/Plug-Ins/Components/EchoJay V2.component` (v2.23.0) with the *same* component triple `aufx EcJ2 Ecjy` as the dev build in `~/Library/...` (v2.23.53). macOS resolves the collision by version, so the newer dev build currently wins — but if a dev build ever carries a version at or below the installed release, Logic will silently load the **release** binary instead. Bump the project version or `sudo rm -rf` the system copy before trusting a test.

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
