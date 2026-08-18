# Webview spike — embed-mode addendum, and the decision

**Measured 18 Aug 2026, ~17:28–17:45 BST**, second Mac (`macbookpro-lan`),
same method as `WEBVIEW_SPIKE_RESULTS.md`. Probe AU 2AC5421C, loading
`https://echojay-dash-preview.vercel.app/dashboard?embed=plugin` directly
(no seed redirect — the gate's 307 strips the query, so the probe relies on the
already-warm `ej_v2_preview` cookie; a `~/.echojay/v2preview.seed` file re-arms
the seed path for a cold store).

Backend: deployment `dpl_E9Bs6jvVaRoJGuFwLqGFsummM3mR`, commit `6654df4` on
`feat/dash-next` (`?embed=plugin`: sidebar and hero hidden pre-paint, visualiser
never started — no GL context, no rAF, no compositing layer; static gradient).
Embed mode confirmed in the shipped CSS/JS and by eye before measuring; the
loaded URL carried `?embed=plugin` in the `EJWebSpike` log on every load.

Thresholds unchanged from the pre-registered table.

---

## Scorecard, both passes

| | Survive | Trouble | Full page | Embed |
|---|---|---|---|---|
| **M1** one instance | <80 MB | >150 MB | 326 MB — FAIL | **~149 MB — ON THE LINE** |
| **M2** eight instances | <400 MB | >600 MB | 1,273 MB — FAIL | **~869 MB — FAIL** |
| **M3** warm re-show (median) | <400 ms | >1 s | 19.65 ms — PASS | not re-run (page-weight-independent) |
| Cold rebuild after editor teardown | — | >1 s | 829 ms | **489 ms** |
| **M4** stability | no crash | any crash | none | none (8 instances again clean) |

## M1 — one instance, embed

Same Logic session throughout (Logic 24081, host 24138). Baseline taken with the
plugin window closed, 60 s drain; WebContent and GPU were gone. **The Networking
process (24186) survived teardown** at ~44 MB — it persists once created, so its
delta is counted, not its size.

| | Baseline | Loaded | Δ |
|---|---|---|---|
| Host | 315.8 | 310.0 | −5.8 (noise) |
| WebContent | — | 88.2 | +88.2 |
| GPU | — | 60.7 | +60.7 |
| Networking | 43.7 | 43.9 | +0.2 |
| | | | **≈149 MB** |

Versus 326 MB for the full page: **embed halved it**, almost entirely in
WebContent (143 → 88). The static background was the right call and this is its
number.

## M2 — eight instances, embed

| | Count | Total |
|---|---|---|
| WebContent | **7** (WebKit pooled two same-origin pages into one process) | 586.1 MB |
| GPU | 1, shared | 142.7 MB |
| Networking | 1, shared | +4.1 MB delta |
| Host delta (315.8 → 451.5) | | +135.7 MB |
| | | **≈869 MB** |

**Marginal cost ~103 MB per additional open dashboard.**

### The GPU finding

Embed never creates a GL context, never starts the rAF loop, never composites an
animated layer — and the GPU process still sat at 60.7 MB for one page and grew
to 142.7 MB at eight (~12 MB/page). **Most of the GPU process is the fixed price
of compositing a WKWebView at all, not the visualiser.** Page slimming cannot
remove it; neither can any `?embed=` change. The per-instance cost is
structural.

---

## The decision the numbers make

Eager 1b — a resident webview per instance — is dead on both passes: even the
slimmed page costs ~103 MB per additional open dashboard, which is WebKit
process architecture, not page weight.

**1b survives in exactly one shape: lazy, single-instance.**

- Construct the webview when the Dashboard tab is selected; destroy it when the
  tab is deselected (or the window closes). Never more than one alive per
  process.
- Costs in that shape, all measured: **~149 MB** while the dashboard is actually
  on screen; **~489 ms** to open it (embed cold rebuild, under the 1 s line);
  **~20 ms** warm re-shows while it stays up (JUCE's reload-on-re-show against a
  live view). Scroll position survives reloads; there is a brief flash.
- `FINDING_webview-lazy-construction.md` is therefore not a mitigation option
  but a hard requirement of the architecture.

This resolves the memory-versus-latency crunch in the main results doc: the
static background pulled the cold rebuild from 829 ms to 489 ms, which makes
teardown-on-deselect affordable. Both levers were needed; neither alone was
enough.

### Standing corrections that travel with this

- The editor is **not** destroyed on a Logic Link-window switch (M3: CTOR 0,
  DTOR 0 over 20 switches; DTOR proven working on window close). Three places
  assert otherwise: `DashPoll.h:19-21`, `PluginEditor.cpp:429-437`, spec §6c.
  §6c's route/scroll persistence is largely unnecessary.
- `GET /api/v2/chains/:id` is **not** the only endpoint returning `state`;
  `GET /api/v2/shares/:slug` is the deliberate second. Spec §2b and §10 both
  need the two-endpoint wording.
- §9d shipped as: `listLimit = 8`, plugin/web branch **removed**.
- The gate's 307 **strips query params** — anything that must survive the
  `?v2preview=` seed redirect will not. Load target URLs directly once the
  cookie is warm.

### Prerequisites before stage 2 starts

1. **Push `feat/dash-next`** on the first Mac — 37 commits ahead, unpushed, and
   the deployed preview depends on it.
2. The measured preview deploy carried the other session's uncommitted
   nav/mixer work; harmless for these numbers, but the next deploy should be
   from committed state only.
3. Windows/WebView2 has a different process model; these numbers are
   macOS/WKWebView. Re-verify there before shipping, not before building.

### Spike disposal

`spike/webview-app`, `spike/webview-dashboard` and `acaf313` have served their
purpose. Delete after this document lands on the branch; the numbers, not the
branches, are the artefact. Restore the day-to-day dev build from
`~/ej-installed-backup-dev` (done / to do), and rotate `V2_PREVIEW_TOKEN` at
leisure — it passed through a chat upload on 18 Aug.
