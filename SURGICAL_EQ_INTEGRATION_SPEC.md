# Surgical EQ — Chain Integration Spec (Route B, final step)

For the local Claude Code session. Read with `SURGICAL_EQ_HANDOFF.md` and `SURGICAL_EQ_DESIGN.md`. This is the step that makes the built-in EQ real inside EchoJay: **selectable from the chain's add-plugin menu**, **hostable like any slot**, **drivable by an AI `eq_bands` move with exact values**, plus **building/installing the AU** so it can be tested in Logic.

This is the only step that edits `ChainHost` and other shared/in-flight files. **Rebase `feat/surgical-eq` onto the latest `feat/session-b-state` before starting**, and keep these edits tight and localized so the merge back is clean.

## Goal (from Sean)
1. Manually add "EchoJay EQ" to a chain from the plugin menu, exactly like adding a hosted third-party plugin (a slot).
2. Have the AI place/dial it by prompt, applied with exact per-band values (bypassing the anchor-table path).
3. Test as an **AU** in Logic.

## Boundary to know before you start (important)
The AI *emitting* an `eq_bands` move is a **server-side** change: the chat prompt/contract that produces `settings_structured` lives in the EchoJay backend (the `/api/chat` service at echojay.ai), **not in this repo**. So this client work makes the EQ:
- fully usable **manually** (add from the menu, edit with the Step A editor), and
- **ready** to receive an AI move — the moment the server emits `{"name":"EchoJay EQ","settings_structured":{"eq_bands":[...]}}` (or a dedicated EQ move), the client applies it exactly.

To let Sean test the prompt path **end to end before the backend is updated**, add a small dev affordance (see §4) that feeds a hand-written `eq_bands` JSON straight into `SurgicalEqProcessor::applyEqBands` from the EchoJay UI (a dev-only text box or a temporary chat command like `/eqtest {...}`), so the exact-apply path is provably working from the app. Clearly mark it dev-only.

## The pieces

### 1. A built-in "device" the chain can host (ChainHost)
The chain currently only loads things it can create via `formatManager_.createPluginInstanceAsync` from a scanned `juce::PluginDescription`, resolved by name (`resolveByName` / `loadByRecommendedName`). A built-in has no scanned description, so add a non-format load path. Mirror the existing in-house-node pattern — `SlotWetBlend` is already a hand-built `juce::AudioProcessor` node in the graph (`ChainHost.cpp:438`).

- Define a canonical name constant, e.g. `"EchoJay EQ"`, and a synthetic `juce::PluginDescription` (stable `fileOrIdentifier`/`uid`, `pluginFormatName = "EchoJayBuiltin"`, `isInstrument=false`). Give it a recognizable manufacturer ("EchoJay").
- Add `ChainHost::loadBuiltinEq(...)` (or generalize the load path) that constructs a `SurgicalEqProcessor`, adds it as a graph node, appends a `ChainSlot` (reuse the existing slot struct/flow from `completeLoad`, `:1405`), `bumpChainRevision()`, `rebuildGraph()`, and re-prepares like a normal slot. It participates in per-slot wet/dry via the existing `SlotWetBlend` wiring automatically.
- Make `resolveByName` / `namesMatchLoose` recognize `"EchoJay EQ"` and route to `loadBuiltinEq` instead of the format manager. This is what lets the AI target it by name too.

### 2. Show it in the add-plugin menu (PluginEditor / ChainListPanel)
Find how the manual "add plugin to chain" list is populated (it comes from `PluginScanner`'s scanned list / catalog, surfaced in `ChainListPanel` in `PluginEditor.cpp`). Inject a synthetic entry for "EchoJay EQ" at the top of that list (a curated/pinned item), whose selection calls the built-in load path from §1 rather than a scanned-plugin load. Keep it visually marked as a built-in (e.g. a badge or section header "EchoJay") so it reads as first-class, not a scanned AU/VST3.

### 3. Editor + state (mostly free)
- **Editor**: selecting the slot already calls `ChainHost::createEditorForSlot(i)` → `proc->createEditor()`, which returns `SurgicalEqEditor`. It auto-embeds through the existing `showInline`/`NativeClip` path. Confirm it sizes to the clip rect (Step A default is 640×420) so it doesn't fall to a pop-out.
- **State**: `SurgicalEqProcessor::get/setStateInformation` already exist (JSON). The main plugin's state cache captures hosted-slot state automatically; the Link's `chainModelToVar` calls `getStateInformation` on the slot. **Restore caveat**: `tryRestoreSlotsFromXml` → `restoreNextSlot` recreates slots via the format manager — add the synthetic-description branch there so a saved built-in EQ slot is rebuilt via `loadBuiltinEq` (a built-in can't be recreated through `formatManager_`). Test: add EQ → set bands → save project → reopen → bands restored.

### 4. Exact AI apply (the crux — bypass the anchor path)
Route structured settings for the built-in EQ straight to the native setter:
- In `setSlotStructuredSettings` / `applyStructuredIfReady` (`ChainHost.cpp:1474/1633`), detect the built-in EQ slot (by the synthetic uid/name) and, instead of the `EchoJayParamApply` anchor pipeline, call `static_cast<SurgicalEqProcessor*>(slotProcessor)->applyEqBands(structured["eq_bands"])`. This is the whole point: exact per-band values, no interpolation, no read-back/revert.
- Accept both shapes: a top-level `eq_bands` array, or `settings_structured.eq_bands`. If the server ever sends flat `settings_structured` for the EQ, ignore it (the EQ only understands `eq_bands`).
- **Dev affordance** (from the Boundary note): add a dev-only entry point in the UI to paste an `eq_bands` JSON and apply it to the selected EQ slot, so the path is testable now. Gate it behind the existing dev flag if there is one, or a clearly-labelled debug control.

### 5. Build + install the AU (so Logic can load it)
Logic is **AU-only** — the VST3 we compile-checked won't show in Logic. Build and install the AU:
```
cd ~/echojay-vst
cmake --build build --target EchoJay_AU -j8
# install for Logic (AU lives here):
cp -R "build/EchoJay_artefacts/Debug/AU/EchoJay V2.component" ~/Library/Audio/Plug-Ins/Components/
# (path may differ by config/JUCE version — find the built .component under build/ and copy it)
```
Then in Logic: it'll rescan (the cache works now), and load the fresh **EchoJay V2** AU with the EQ available in its chain menu. Note the bundle is code-unsigned for local dev; if Logic refuses it, `xattr -dr com.apple.quarantine` the copied `.component` (it's local, so usually fine). Optionally flip `COPY_PLUGIN_AFTER_BUILD` to `TRUE` for the dev build so future builds auto-install, but don't commit that flip if it affects release builds — keep it a local/dev-only change or a separate CMake option.

## Constraints (unchanged)
- Keep `EqEngine`/`EqMove` JUCE-free and their g++ tests green.
- No APVTS. House-style JSON state.
- Localize the `ChainHost`/`PluginEditor` edits; land them in one tight pass rebased on latest `session-b-state`.
- Commit/push to `feat/surgical-eq` with the project trailer. Suggested commits: (a) ChainHost built-in load path + resolveByName + restore branch, (b) add-menu entry, (c) exact-apply routing + dev affordance, (d) AU build/install notes if any CMake dev option is added.

## Acceptance
In Logic, loading **EchoJay V2** (AU): "EchoJay EQ" appears in the chain's add-plugin menu; adding it inserts a working EQ slot whose editor opens inline; editing bands changes the audio; a pasted `eq_bands` JSON (dev affordance) applies exact per-band values; saving/reopening the project restores the EQ. The g++ unit tests still pass.
