# Surgical EQ — Client follow-ups to unlock the backend

The backend (`feat/eq-backend` on echojay-saas) is implemented but **gated off** behind two version pins, because a built-in device can't be advertised through the client's plugin feed and the edit path needs the client to carry the move. These two small plugin-side changes remove both gates. **They ship together in one build** — then the backend keys both behaviours off the client's advertisement instead of version numbers.

Branch: `feat/surgical-eq`. Keep `EqEngine`/`EqMove` JUCE-free and their g++ tests green. Commit/push in focused commits, rebuild + reinstall the AU.

---

## Part 1 — Advertise built-in devices (`[AVAILABLE BUILTINS]`)

**Why:** the server only sees the user's *scanned* plugins (the client injects `[AVAILABLE PLUGINS …]`). The EchoJay EQ is built-in, so it can never appear in that feed — the backend currently guesses support from plugin version, which is fragile (feat/surgical-eq is 2.23.53, unrelated feat/session-b-state is 2.23.55, only the former has the EQ). Instead, have the client explicitly advertise its built-in devices; the backend gates on that.

**Change (client):**
1. Add a source of truth for built-in device names. In `ChainHost`, add a small getter, e.g. `static juce::StringArray builtinDeviceNames() { return { kBuiltinEqName }; }` (today just `"EchoJay EQ"`; future built-ins append here).
2. In `EchoJayAPI::buildPluginInjection` (`EchoJayAPI.cpp:1760`), where it already builds the `[USER'S FULL PLUGIN LIST …]` and `[AVAILABLE PLUGINS …]` blocks, append one more block with the **exact marker** the backend will parse:
   ```
   \n\n[AVAILABLE BUILTINS — EchoJay's own built-in devices, always available regardless of installed plugins; use in a chain by exact name]:\nEchoJay EQ
   ```
   Comma/newline-separated if more than one later. Inject it whenever the plugin feed is injected (chain-capable turns) — it costs one short line.
3. Update the cache-split marker list at `EchoJayAPI.cpp:924` (the array that currently lists `"\n\n[AVAILABLE PLUGINS"`, `"\n\n[USER'S FULL PLUGIN LIST"`, …) to include `"\n\n[AVAILABLE BUILTINS"` so the prompt-cache breakpoints treat it consistently.

**Marker contract (client ↔ backend must agree):** the block starts with `[AVAILABLE BUILTINS` and lists exact device names, one per line (or comma-separated). The backend offers a built-in device only when its exact name appears here.

**Backend counterpart (relay to the echojay-saas session):** replace the `EQ_DEVICE_MIN_PLUGIN_VERSION` gate with a parse of the `[AVAILABLE BUILTINS]` marker — offer "EchoJay EQ" iff it's present in the feed. No version guessing; older clients (no marker) simply never get it offered.

---

## Part 2 — Carry `settings_structured` (`eq_bands`) on chain-edit ops

**Why:** the client applies `settings_structured` on the **build** path (`<<<ECHOJAY_CHAIN>>>` → `loadChainFromJson`, which sets it per slot at `PluginEditor.cpp:17782`), but **not** on the **edit** path — the code says so at `PluginEditor.cpp:14950` ("no settings_structured rides an op"). So "add an EQ and cut 400 Hz" (an edit turn) places the EQ but never dials it. The backend already emits and validates `settings_structured` on edit blocks (gated behind `EQ_EDIT_MIN_PLUGIN_VERSION`); the client just needs to carry it through.

**Change (client):**
1. Add a field to the edit-op struct: in `ChainEditOp` (`ChainHost.h:140`), add `juce::var structuredSettings;`.
2. In `parseChainEditOps` (`ChainHost.cpp:963`), for `add` and `replace` ops read `settings_structured` off the op object into `op.structuredSettings` (same key the build path reads).
3. In the edit sequencer `runNextEditOp` (`ChainHost.cpp:1158`), after an `add`/`replace` op's slot has **finished loading**, call `setSlotStructuredSettings(newSlotIndex, op.structuredSettings)` — mirroring exactly what the build path does after each slot loads. That routes through the existing `applyStructuredIfReady`, which already dispatches the built-in EQ to `applyEqBands` (exact) and third-party slots to the anchor path (unchanged) — so this is consistent with build-path behaviour, not new apply logic.
4. Update the stale comment at `PluginEditor.cpp:14950` since settings_structured now rides edit ops.

**Backend counterpart:** flip the `EQ_EDIT_MIN_PLUGIN_VERSION` gate. Since both client changes ship in the same build, the simplest is to key the edit path off the **same `[AVAILABLE BUILTINS]` advertisement** as the device gate (a client that advertises the built-in is by definition a build that also carries eq_bands on edits) — dropping both version pins in favour of the one capability signal.

---

## Sequencing & acceptance
- Land both client changes in one build; push `feat/surgical-eq`; rebuild + reinstall the AU.
- Relay the two "backend counterpart" notes to the echojay-saas session so it swaps both version gates for the `[AVAILABLE BUILTINS]` capability parse, then preview-deploy.
- **Acceptance:**
  1. On the preview backend, with the new plugin build, the model offers/uses "EchoJay EQ" **because the client advertised it** (not because of a version number).
  2. "add an EQ and take 3 dB out of the boxiness around 400" (an **edit** on an existing chain) adds an EchoJay EQ and **dials** the −3 dB @ 400 band exactly — verifiable on the EQ curve/readouts.
