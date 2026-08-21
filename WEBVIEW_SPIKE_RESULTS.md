# Webview spike — results

**Measured 18 Aug 2026** on the second Mac (`macbookpro-lan`), Logic Pro,
AU, arm64. Probe: `spike/webview-dashboard`, JUCE 8.0.12,
`JUCE_WEB_BROWSER=1`, `WKWebView` loading the full
`https://echojay-dash-preview.vercel.app/dashboard`.

**Label:** full `/dashboard`, **signed in and populated**, hero and sidebar
included — `?embed=plugin` does not exist, so this is the whole page. Host
process is `AUHostingServiceXPC_arrow` (out-of-process AU hosting).

Thresholds were fixed **before** measuring and are unchanged below.

---

## Scorecard

| | Threshold: survives | Threshold: trouble | Measured | Verdict |
|---|---|---|---|---|
| **M1** one instance | under ~80 MB | over ~150 MB | **326 MB** | **FAIL** — 2.2× the trouble line |
| **M2** eight instances | under ~400 MB | over ~600 MB | **1,273 MB** | **FAIL** — 2.1× the trouble line |
| **M3** re-show latency | under ~400 ms | over ~1 s | **19.65 ms** median | **PASS** — comfortably |
| **M3** visual | brief flash | white rectangle | brief flash, scroll survives | **PASS** |
| **M4** stability | no crash over 20 | any crash / leak | none | **PASS** |

**Memory fails decisively. Latency and stability pass comfortably.**

---

## M1 — one instance

Same Logic session throughout (Logic pid 18115, host pid 18179). Baseline taken
with the plugin window **closed** and a full minute allowed for WebKit's process
reuse cache to drain — confirmed by `EJWebSpike editor DTOR` at 16:25:19 and the
absence of any WebKit row.

| | Baseline | Loaded | Δ |
|---|---|---|---|
| `AUHostingServiceXPC_arrow` | 176.7 MB | 263.5 MB | **+86.8** |
| WebKit `WebContent` | — | 143.0 MB | +143.0 |
| WebKit `GPU` | — | 69.2 MB | +69.2 |
| WebKit `Networking` | — | 26.9 MB | +26.9 |
| | | | **+325.9 MB** |

Logic itself moved 505.2 → 508.9 MB (noise).

Fails on the conservative accounting too: counting only the WebKit helpers and
discarding the host's 87 MB growth still gives 239 MB, well over the 150 MB
trouble line.

---

## M2 — eight instances

| | Count | Total |
|---|---|---|
| `WebContent` | **8** (one per instance) | 795.8 MB |
| `GPU` | **1** (shared, same pid as M1) | 181.3 MB |
| `Networking` | **1** (shared, same pid as M1) | 37.6 MB |
| Host delta (176.7 → 435.0) | | 258.3 MB |
| | | **1,273.0 MB** |

### Linearity: sub-linear, and it does not save you

- **GPU and Networking are shared.** One of each across all eight, the same pids
  that served the single instance in M1. That is the good case §6d hoped for.
- **`WebContent` is one process per instance.** No sharing.
- 8 × M1 would be 2,607 MB. Actual is **1,273 MB — 49% of linear.**
- Individual `WebContent` processes shrink as instances multiply (143 MB alone,
  then 75–131 MB each), because eight copies of the same origin share cached
  resources.

**Marginal cost: the first open dashboard costs ~326 MB; each additional one
costs ~135 MB.**

So the sharing halves the bill and the bill is still twice the budget.

---

## M3 — re-show latency, and a correction to a standing belief

20 switches between the Link window and EchoJay, timestamp-anchored so nothing
from M1 or the seed load is included.

```
n=20  min=7.4  median=19.65  max=27.6      (milliseconds)
CTOR: 0
DTOR: 0
```

### The editor is NOT destroyed on a Link-window switch

Zero constructions and zero destructions across all twenty switches. The
instrumentation works — `DTOR` fired correctly at 16:25:19 when the plugin
window was closed.

**This contradicts a belief asserted in three places:** `DashPoll.h:19-21`,
`PluginEditor.cpp:429-437`, and the parity spec's §6c. The accurate statement
appears to be: **closing the plugin window destroys the editor; switching to the
Link window and back does not.** Someone likely observed the first and
generalised to the second.

Consequences:

- §6c's "persist route, scroll and half-typed message on the processor" is
  largely unnecessary for this path.
- Scroll position survives the reload anyway — WebKit restores it.
- `DashPollShared` living on the processor remains harmless and is still the
  right shape, but its stated justification is weaker than believed.

### What the 20 ms does and does not measure

**It measures re-showing a webview that stayed alive** — JUCE reloading the URL
into an existing `WKWebView` with warm processes and hot caches. See
`FINDING_webview-reloads-on-reshow.md`: `checkWindowAssociation()` calls
`reloadLastURL()` on every re-show, so a full page load happens on every tab
re-selection, not only on window switches.

**It does not measure teardown and rebuild.** The cold figure is already on
record from the seed load: **829 ms**, roughly 40× the warm figure and right on
the 1 s trouble line.

That distinction is the whole problem — see below.

### Visual

A perceptible flash on every switch. Scroll position survives. Not a threshold
failure, but it would happen every time anyone glances at the Dashboard and
back, permanently.

---

## M4 — stability

Nothing crashed, hung, or leaked. Eight instances reached without incident, no
climbing `WebContent` count.

---

## What this means for the 1a/1b decision

**The mitigation and the measurement point in opposite directions.**

The only way to stop paying 326 MB per instance is lazy construction plus
teardown when the Dashboard tab is deselected — cap it at one live webview.
But the rebuild on return is a *cold* start, ~829 ms, not the warm 20 ms. So:

| Strategy | Memory | Latency returning to the tab |
|---|---|---|
| Eager, keep alive (as measured) | 326 MB × instances, 1.27 GB at eight | 20 ms |
| Lazy + teardown on deselect | ~326 MB peak, one at a time | ~829 ms, every time |

You can have the memory or the latency, not both. The reload-on-re-show
behaviour is inherited from JUCE and is not cheaply removed.

### Options

1. **1b with lazy construction and teardown.** Accept ~326 MB whenever the
   dashboard is visible and a ~0.8 s wait each time it is opened. Viable. The
   memory is still large for one tab of one plugin, and a producer with several
   EchoJays who opens several windows pays it repeatedly.
2. **1b eager.** Rejected by M2 — 1.27 GB at eight instances is not defensible
   in a mixing session.
3. **Hybrid, inverted from §1c.** Native for the dashboard body (which the
   plugin already has and which costs nothing), webview *only* for the parts
   that are genuinely expensive to rebuild — messaging and the community panel —
   opened on demand and torn down after. Pays the 326 MB only when someone is
   actually reading messages, which is rare, and keeps the common case free.
4. **1a native.** Now more attractive than it looked. The measured cost of 1b is
   not the memory alone but memory *plus* a permanent flash on every tab switch
   *plus* the ongoing parity bill that 1a was rejected for.

**Recommendation: option 3.** It preserves §1c's actual argument — that
messaging is the largest chunk of 1a and the least valuable to rewrite — while
declining to pay webview cost for bands the native tab already renders well. The
dashboard body is mostly lists and tiles, which `DashboardTab.cpp` already draws
in ~1,200 lines; the community panel with ten tabs, two trays, reactions and
search is where 1a's bill actually lands.

This is a product call, not a technical one. The numbers say 1b is affordable
only if the webview is rare and short-lived; option 3 is the shape that makes it
rare and short-lived.

---

## Still open

- **`?embed=plugin` does not exist** (`FINDING_dashboard-preview-gate.md`).
  Everything above measures the full page with hero and sidebar. An embed mode
  would trim it, but not by a factor of three — the SPA bundle dominates.
- Whether these figures hold on Windows with WebView2, which has a different
  process model.
- The spike branches (`spike/webview-app`, `spike/webview-dashboard`) and
  `acaf313` are throwaway and should be deleted once this document is accepted.
