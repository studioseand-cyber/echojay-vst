# Surgical EQ — Resume note (pick up here)

Snapshot at end of 28 Jul session. Read this first, then `SURGICAL_EQ_HANDOFF.md` for depth.

## Where it stands — DONE
- **Plugin (`feat/surgical-eq`, pushed):** the EQ is feature-complete and working in Logic as an AU.
  - DSP (`EqEngine`) + dynamic bands, all g++-tested.
  - Editor: EchoJay logo, inline hosting in the chain, filmstrip dials with readouts, Pro-Q-style analyzer with its **own right-hand dBFS axis** + **PRE/POST** toggle + dynamic-band metering.
  - Chain integration: "EchoJay EQ" is a first-class built-in device (add-menu, hosted slot, exact apply via `applyEqBands`, state/restore). `/eqtest {json}` dev command applies a move by hand.
  - Client follow-ups landed (commits `c1b8513`, `7523eec`): advertises `[AVAILABLE BUILTINS: EchoJay EQ]` in the plugin feed, and carries `settings_structured`/`eq_bands` on chain-**edit** ops so edits dial too.
- **Backend (`echojay-saas`, branch `feat/eq-backend`):** implemented — `eq_bands` validation (`lib/validate-settings.js`), device registry + prompt notes (`api/_builtin-devices.js`), auto-dial carve-out, telemetry fixes (recent-plugins avoid-list + violation logger), scrubber fix, edit-block validation. Tests green (128 new).

## ⚠️ Do first tomorrow — the at-risk item
**The backend work is NOT committed.** On the build Mac, in `ECHOJAY FILES/ECHOJAY WEB APP/echojay-saas` (branch `feat/eq-backend`), commit it before anything else so it can't be lost.

## Remaining steps (in order)
1. **(Optional, recommended) plugin one-liner:** advertise built-ins even when the scanned-plugin list is empty (the EQ is always available and shouldn't require owned plugins) — one-line change to the early return in `EchoJayAPI::buildPluginInjection`. Rebuild + reinstall the AU.
2. **Backend gate-swap** (echojay-saas session): the device is currently **gated OFF** behind two placeholder version pins. Swap both for the capability advertisement:
   - Replace `EQ_DEVICE_MIN_PLUGIN_VERSION` with a parse of the `[AVAILABLE BUILTINS` marker — offer "EchoJay EQ" iff its exact name appears in the feed.
   - Key `EQ_EDIT_MIN_PLUGIN_VERSION` off the same advertisement (a client that advertises the built-in also carries `eq_bands` on edits).
   Commit.
3. **Preview-deploy** the backend.
4. **Test on preview** with the new plugin build:
   - Acceptance: "add an EQ and take 3 dB out of the boxiness around 400" (an **edit** turn) → adds an EchoJay EQ **and dials** the −3 dB @ 400 band exactly (check the curve/readouts).
   - Prompt batch: confirm the model reaches for the EchoJay EQ sensibly for surgical moves (the when-to-use guidance is **untested against the live model** — tune it here before production).
5. **Ship:** merge backend to production; merge `feat/surgical-eq` into the release line and cut a build. Because gating is now on the `[AVAILABLE BUILTINS]` capability (not version), there's **no version-pin discipline to worry about** — older builds simply never advertise it.

## Contract (both sides agree on this)
- Client advertises: `[AVAILABLE BUILTINS — …]:\nEchoJay EQ` in the plugin-feed injection.
- Move rides as `settings_structured.eq_bands` on the "EchoJay EQ" chain entry (build path today; edit path once both land in one build — they now have).
- `eq_bands` schema: `SURGICAL_EQ_BACKEND_SPEC.md` (authoritative).

## Loose ends / notes
- **Duplicate-AU hazard** still stands: root-owned release `EchoJay V2.component` (v2.23.0) vs dev build. `sudo rm -rf` the system copy if a test ever looks wrong.
- Pre-existing `SCRUB_HEADERS` drift in the backend (unrelated headers unscrubbable) — left alone on purpose; separate cleanup someday.
- Two Macs: plugin work on the surgical-eq Mac (`~/echojay-vst`... actually `ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200` on the build Mac has it too via fetch); backend on the build Mac (`ECHOJAY FILES/ECHOJAY WEB APP/echojay-saas`).
- Nice-to-haves parked: resonance-hunt helper, linear-phase mode, mid/side per band, presets, output/auto-gain, note tooltip on drag.
