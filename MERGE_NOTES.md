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

**THIRD CALLER (stage 3, the loadChain bridge).** `EchoJayEditor::bridgeOpenChainById`
(`PluginEditor.cpp`) now calls the **two-argument** `openSavedChain(chainId, name)`
as the convergence point for dashboard-row loads. When the other branch's
`onFetchError`/`onSlotsParsed` parameters land, **this call site needs the same
treatment as the confirm re-entry above** — either pass the two callbacks
through, or deliberately pass `{}` if a bridge load wants no recall reporting —
but decide it, do not let the defaulted-parameter merge silently drop them. It is
the same silent-merge shape: it compiles, produces no conflict marker, and only
misbehaves at runtime.

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

## 4. §7 offline chains list — consciously traded away (stage 3, item 2)

The native `DashboardView` was §7's offline story: a disk-cached chains list you
could still act on with no network. Item 2 retires that view from display — it
now renders **only** the signed-out line. Signed-in, the webview is the surface;
while it builds or after it fails, a minimal native panel stands in ("Loading
your dashboard…" / "You're offline — go online to view your dashboard"), NOT the
cached list.

So **§7's "a chain you can load offline" is gone by product-owner decision.** The
code (`DashboardTab.cpp`, `dashView_`, `tools/dashboard_test`) is still present
and shipped — this is retirement-from-display only; deleting it wholesale is a
separate, explicitly-approved commit. The redundant `openDashboardTab` fetch into
the now-hidden `dashView_` is left in place for that small-diff reason and can be
removed with the deletion.

## 5. Chat sidebar deep-link scroll — a new method + two one-line call sites

Fixing "a dashboard deep link selects a chat but the sidebar doesn't scroll to
it" adds a small method and calls it at exactly two sites, all in the chat-sidebar
code the other machine (`integration/reasoning-plus-pitch`) may also touch:

- **New method** `EchoJayEditor::scrollChatSidebarToActive()` — decl in
  `PluginEditor.h` right after `loadChatFromWorkspace`; impl in `PluginEditor.cpp`
  right *before* `loadChatFromWorkspace`. It scans `sidebarModel->rows` for the
  `ChatRow` whose `id == currentChatId` and positions `chatSidebar`'s viewport so
  that row lands in the top third. Pure read of existing state — if it collides,
  keep one copy; there is nothing to reconcile.
- **Two call sites**, each a single statement appended after an existing
  `loadChatFromWorkspace(...)` call, inside the SAME `if (ch.id == …) { … }`:
  - `followDashLink` chat branch (immediate path, workspace already loaded).
  - the `pendingDashChatId_` consumption in `workspace.onLoaded` (parked path).
  If either `loadChatFromWorkspace(...)` line moves or is rewritten on the other
  branch, re-append `scrollChatSidebarToActive();` after it.

Deliberately NOT wired into `loadChatFromWorkspace` itself: that is also the
manual sidebar-click path, where the row is already on screen and an auto-scroll
would be a jarring jump. The scroll is scoped to the deep-link entries only.

The **editor-recreate restore path** (`if (ch.id == restoreId) …` a few lines
above the parked block) was left untouched — same "which chat am I on" benefit
would apply, but it is outside this fix's stated scope; add the same one-liner
there if that becomes desired.

The active-row styling was assessed and left as-is: an active `ChatRow` already
draws a lighter `bg4` fill, a 3px `C::blue` left accent bar, and brighter text —
unambiguous once on screen. The bug was purely off-screen position, not styling.
