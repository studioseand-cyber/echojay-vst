# Finding: `juce::WebBrowserComponent` reloads the whole page on every re-show — a full `/dashboard` fetch per window switch

**For the 1b (WebView) decision, alongside the spike numbers.** Found
2026-08-18 while fixing M3's instrumentation. This is core JUCE behavior, not a
probe bug, and it is arguably the sharpest cost in the whole 1b question.

## The observation

M3 (20 Link-window switches) logged **17 page loads with the editor never
recreated** (`EJWebSpike editor CTOR` count = 0). The probe issues `goToURL`
exactly once, in the editor constructor. Something else reloaded the page 17
times.

## The mechanism (JUCE)

`WebBrowserComponent::parentHierarchyChanged()` and `::visibilityChanged()` both
call `checkWindowAssociation()` (`juce_WebBrowserComponent.cpp:688-695`). The mac
implementation (`juce_WebBrowserComponent_mac.mm:734`):

```cpp
void checkWindowAssociation() override
{
    if (browser.isShowing())
        browser.reloadLastURL();          // full goToURL(lastURL) — page re-fetch + re-render
    else
        // optionally force about:blank when hidden (unloadPageWhenHidden)
}
```

So **every time the component becomes "showing" again, JUCE reloads the entire
page.** In Logic, a Link-window switch detaches and reattaches the plugin
editor's NSView (`parentHierarchyChanged`) and toggles the Dashboard tab's
visibility (`visibilityChanged`) — either fires `checkWindowAssociation` → a full
reload. The same happens on tab re-selection and on editor re-open.

## Probe artefact? No — inherited

The probe calls `goToURL` once; the reloads come from JUCE. So **any 1b build
using `juce::WebBrowserComponent` inherits this**: the 499 KB dashboard SPA and
its data re-fetch and re-hydrate on **every window switch and every tab
re-selection**, not just on first open. That is a full `/dashboard` fetch every
time the tab is touched.

## Why it matters, and how it compounds the other findings

- **Perf/UX:** constant reloads mean the dashboard flashes and re-fetches on
  routine Logic interaction (window switches happen constantly), with the network
  cost of §6a (5 Redis + 4 Postgres) each time if the page re-requests its
  payload, plus a full SPA re-parse.
- **Compounds lazy construction** (`FINDING_webview-lazy-construction.md`): even a
  webview built lazily on first Dashboard selection will, once it exists, reload
  on every subsequent re-show. Lazy construction bounds *how many* webviews exist;
  this bounds *how often each one reloads* — both need solving.
- **M3 is really measuring this reload.** With the instrumentation fixed (t0 at
  load start, not editor construction), M3's `dt_ms` is now the honest cost of one
  JUCE-triggered reload after a switch — which is the number that matters, because
  it happens on every switch.

## What 1b needs

A webview host that does **not** reload on re-show — either suppress
`checkWindowAssociation`'s `reloadLastURL` (keep the page resident across
hide/show and reparent), or an integration that isn't `juce::WebBrowserComponent`
as-shipped. This must be designed in before 1b, not discovered in QA.

## Evidence

- `juce_WebBrowserComponent.cpp:688-713` (`parentHierarchyChanged`,
  `visibilityChanged`, `reloadLastURL`), `juce_WebBrowserComponent_mac.mm:734`
  (`checkWindowAssociation` → `reloadLastURL` when showing).
- Probe: single `goToURL` in the editor ctor; M3 logged 17 loads at CTOR=0.
