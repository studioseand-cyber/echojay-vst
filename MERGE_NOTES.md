# Merge notes — `feat/dashboard-tab`

Written for whoever merges this branch with
`integration/reasoning-plus-pitch` (the second Mac, audio path). This branch
is the state-chunk-matching + deadman work (CHAINHOST_BRIEF and
CHAINHOST_FOLLOWUP_BRIEF). It changes `ChainHost.{cpp,h}` substantially and
`PluginEditor.{cpp,h}` by a few lines only.

## 1. SILENT MERGE HAZARD — `openSavedChain` loses its recall hooks (ACT ON THIS)

`integration/reasoning-plus-pitch` widens the signature to the **recall** path:

```cpp
// origin/integration/reasoning-plus-pitch, PluginEditor.h:1419 / .cpp:27065
void openSavedChain(const juce::String& id, const juce::String& name,
                    std::function<void(int statusCode, const juce::String& err)> onFetchError = {},
                    std::function<void(const juce::StringArray& intendedNames)> onSlotsParsed = {});
```

This branch adds, at the **top** of `openSavedChain`, an unsaved-rack confirm
that **re-enters `openSavedChain` after the user clicks Replace**:

```cpp
// this branch, PluginEditor.cpp — inside the confirm's modal callback (~line 25182)
safeThis->openSavedChain(id, name);   // TWO-ARGUMENT re-entry
```

A `git merge-tree` dry run of the two branches produces **zero conflict
markers** — which is exactly the danger. The merge compiles (the new
parameters are defaulted), and the re-entry silently calls the two-argument
form, so **a recall that passes through the confirm loses its `onFetchError`
and `onSlotsParsed` hooks** — its error reporting and its slots-parsed hook —
with nothing in the diff to show it.

**When merging:** thread the two callbacks through the confirm. Capture them
in the modal-callback lambda and forward them on the re-entry:

```cpp
safeThis->openSavedChain(id, name, onFetchError, onSlotsParsed);
```

(and add `onFetchError, onSlotsParsed` to that lambda's capture list). The
confirm block itself merges cleanly; only the re-entry call needs the hands-on
fix.

## 2. Left alone deliberately — pre-existing, not introduced here

Both were found while doing this work and correctly left untouched. Recorded
here so they are not rediscovered from scratch.

- **Deadman cross-sequence race.** `loadPluginAsync` writes the thin-VST3
  validation marker (`ChainHost.cpp:3180`, `identifier\nload`) and
  `applyRestoredState` writes the state-restore marker
  (`ChainHost.cpp:4502`, `identifier\nstate restore`) to the **same file**
  (`chain_load_deadman.txt`). Within one restore sequence they are strictly
  ordered (message-thread only; a slot finishes loading before its state is
  applied). But **two independent load sequences at once** — a session restore
  still in flight when the user opens a saved chain or an AI build starts —
  can interleave: sequence 2's state-restore write overwrites sequence 1's
  pending validation marker and then deletes it, so sequence 1's validation
  runs uncovered and a crash in that window is misattributed. No torn write
  (both are message-thread only); it is lost coverage / misattribution.
  Cheap mitigation if ever wanted: delete the marker only when it still holds
  what this writer wrote.

- **`loadBuiltinNow` reads a moved-from name.** `ChainHost.cpp:3103` logs
  `slot.desc.name` after `slots_.push_back(std::move(slot))` a few lines
  above, so the built-in's name prints empty (`ChainHost: built-in "" added
  as slot N`). Log-only; no functional effect. Capture the name before the
  move to fix.

## 3. Windows/WebView2 — a designed fallback is required before Windows ships (stage 2)

Stage 2 flipped `JUCE_WEB_BROWSER=0 -> 1` and added the lazy webview Dashboard
(`Source/DashboardWeb.*`, `PluginEditor` Dashboard blocks). **Everything measured
and implemented is macOS/WKWebView.** On Windows, `juce::WebBrowserComponent` uses
**WebView2, which requires the Evergreen Runtime** — pre-installed on Windows 11
and most Windows 10 since 2021, but **not guaranteed** (parity spec §1b item 1).

Before shipping Windows, this needs a **presence check at editor construction and
a designed fallback — never a blank rectangle**: either the native-lite tab
(the native `DashboardView` is already the fallback surface — signed-out /
offline / load-failed — so wiring "runtime absent" to it is small) or an honest
"open in browser" panel. `DashboardWeb`'s `onLoadResult(false)` already routes a
failed webview to the native view; a WebView2-runtime-absent check should route
the same way (skip construction entirely, like the signed-out case in
`reconcileDashboardWeb`). Out of scope for the macOS stage — flagged here so
whoever builds the Windows target does not discover it as a blank rectangle in QA.
