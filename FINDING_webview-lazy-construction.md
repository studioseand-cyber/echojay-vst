# Finding: 1b must build the webview lazily (on first Dashboard selection) and tear it down when idle

**For the 1b (WebView) decision, alongside the spike numbers.** Recorded
2026-08-18 from the webview measurement probe. This is the larger half of the
memory story — bigger than the raw M1/M2 figures.

## The observation

The probe constructs the `WKWebView` and calls `goToURL("/dashboard")` in the
**editor constructor**. So the full webview cost — the out-of-process
`WebKit.WebContent` / `WebKit.GPU` / `WebKit.Networking` helpers plus the 499 KB
SPA load — is paid on **editor-open, for every instance, whether or not the user
ever selects the Dashboard tab**.

Eight EchoJays on eight tracks = **eight WKWebViews and eight page loads with
nobody looking at a dashboard.** That multiplier is paid by users who never open
the Dashboard at all.

## Why this is a different, bigger lever than §6d

§6d anticipated **focus-gating the polling** — throttling the 20 s community
poll / payload refresh on unfocused instances. That saves network and CPU on the
*refresh cadence* of a webview that already exists.

Lazy construction is a different axis: it decides **whether the webview exists at
all**. Not constructing it saves an entire `WebContent` process (and its GPU /
Networking share, and the page load) per idle instance — memory, not just
refresh traffic. On a loaded session (§6d's own concern) this dominates.

## The requirement for 1b

1. **Construct and load the WKWebView on first Dashboard-tab selection**, not on
   editor-open — i.e. inside `openDashboardTab()`, exactly where the native
   dashboard already triggers its own expensive work (see below).
2. **Tear it down after the tab has been unselected for a while**, reclaiming the
   `WebContent` process, and reconstruct on next selection. The default
   `WKWebsiteDataStore` is persistent and process-shared, so the auth session and
   the `ej_v2_preview` gate cookie survive teardown — reconstruction reloads
   populated, no re-login, no re-seed.

## Probe artefact, not inherited shape

The eager load is a **probe artefact**, not the shape the real thing inherits.
The native dashboard already does the right thing:

- `dashViewport_` and `dashView_` are **cheap eager members** (a Viewport and a
  Component — no network, no heavy resources at construction).
- The **expensive work** — `GET /api/v2/dashboard?surface=plugin` (§6a: 5 Redis +
  4 Postgres) — is gated to `openDashboardTab()`, called from
  `switchToTab(Tab::Dashboard)` and from the constructor **only when the editor
  opens on the Dashboard tab** (`PluginEditor.cpp:2526`). An editor that opens on
  any other tab fetches nothing.

So 1b is **not forced** to pay the webview cost eagerly. It can and should place
the WKWebView's construction and load at the same seam the native payload fetch
already uses (`openDashboardTab`), and add teardown-when-idle on top — which the
native cheap-when-idle members never needed.

## How to read the M1/M2 numbers in light of this

M1/M2 are measured with the webview **eagerly loaded and populated**, so they are
the cost of **one open, populated dashboard**. Without lazy construction, that
cost is multiplied across every instance regardless of use. With lazy
construction, an instance whose Dashboard is never opened pays **zero** webview
cost — which is the whole point of raising this before 1b is built.

## Stage 2: the webview is a native NSView, so overlays must HIDE it, not out-z-order it

In 1b the webview is a `WKWebView` — a **native NSView**, which composites above
**all** JUCE lightweight painting unconditionally, regardless of z-order (the
same rule the Chain rack already lives by: hosted-plugin NSView editors "must be
HIDDEN, not out-z-ordered"). So `toFront()` on an overlay does nothing against
it: **every prompt and overlay that should outrank tab content must drive the
webview's `setVisible(false)`, never rely on paint order.** This is not
hypothetical — the lightweight `dashViewport_` already slipped past
`onboardingOverlay_.toFront()` and covered the intake prompts on every fresh
instance (fixed by folding the prompt/overlay set into its single visibility
expression). A native webview makes that failure mode unconditional. The hook is
already in the right place: the `dashViewport_.setVisible(...)` expression in
`resized()` that this work extends is exactly where 1b swaps in the webview, so
the guard set (channel/genre/project prompts, update overlay, review overlay, and
any future overlay) travels with it.
