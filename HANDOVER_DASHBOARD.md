# Handover: the plugin Dashboard, end to end

*Branch `feat/dashboard-tab` in `~/echojay-vst`. Written 19 Aug 2026 to replace
the build conversation as the one place this system exists as a single story.
Read it as someone with neither that chat nor the other machine's history — you
in three months, or the `echojay-saas-dash` session tomorrow. Every claim here
is either verifiable from this repo (the findings, the two spike-results docs,
`MERGE_NOTES.md`, `CONTRACT_dashweb_bridge.md`, the parity spec, `git log
7cff677..HEAD`) or, where it concerns the page repo I cannot see from here,
marked `[saas-side, as reported]`. No secrets: token file paths appear, token
values never.*

---

## 1. What shipped

Between 18 and 19 Aug 2026, across two repositories — `echojay-vst` (this
plugin) and `echojay-saas-dash` (the web app, `[saas-side, as reported]`) — the
plugin's **Dashboard tab became the real v2 web dashboard**, rendered in a lazy
`WKWebView`, fronted by a narrow native bridge for the three things only the
plugin can do: **load a chain** into the rack with every slot's settings
dialled, **open a chat** in the plugin's own Chat tab, and (a later stage) light
the tab **badge**. Everything else the dashboard is or becomes — messaging,
community, the feed, search, profile pages, and every future web change —
arrives in the plugin with **no plugin work at all**, because it is just the
website. The plugin stopped being a second implementation of the dashboard and
became a host for the first one.

---

## 2. The architecture, and the measurements that chose it

The shape is a **lazy, single-instance webview**: constructed when the Dashboard
tab is selected, destroyed when it is deselected (or the window closes), and
**never more than one alive per plugin process**. It is hidden by the *same*
visibility expression that guards the intake prompts and overlays, because a
`WKWebView` is a native `NSView` that composites above all JUCE lightweight
painting **unconditionally** — `toFront()` on an overlay does nothing against it,
so the only correct hide is `setVisible(false)` (this is the same rule hosted
plugin editors already live by; the lightweight `dashViewport_` had already
slipped past `onboardingOverlay_.toFront()` once and covered the intake prompts,
fixed in `0de7629`).

That shape was not a preference; the numbers forced it. A throwaway probe
(`spike/webview-*`, now deleted) measured a live `WKWebView` loading the preview
dashboard on the second Mac in Logic, AU, arm64, with thresholds **fixed before
measuring**:

| Measurement | Survives | Trouble | Full page | Embed (`?embed=plugin`) |
|---|---|---|---|---|
| M1 — one instance | <80 MB | >150 MB | **326 MB — FAIL** | **~149 MB — on the line** |
| M2 — eight instances | <400 MB | >600 MB | **1,273 MB — FAIL** | **~869 MB — FAIL** |
| M3 — warm re-show (median) | <400 ms | >1 s | **19.65 ms — PASS** | (page-weight independent) |
| Cold rebuild after teardown | — | >1 s | 829 ms | **489 ms** |
| M4 — stability (8 instances) | no crash | any crash | none | none |

Read that top-down: a resident webview **per instance** is dead on both passes —
even the slimmed embed page costs ~103 MB for each additional open dashboard,
which is WebKit's one-`WebContent`-process-per-page architecture, not page
weight (GPU and Networking are shared; `WebContent` is not). 1b survives in
exactly one shape: lazy and single-instance. **Two levers together** made even
that affordable, and neither alone was enough:

- **`?embed=plugin`** — the page hides its hero and sidebar pre-paint and never
  starts the visualiser (no GL context, no rAF, no compositing layer; static
  gradient). This halved M1, almost entirely in `WebContent` (143 → 88 MB).
- **Lazy construction + teardown-on-deselect** — an instance whose Dashboard is
  never opened pays **zero** webview cost. The static background also pulled the
  cold rebuild from 829 ms to **489 ms**, under the 1 s line, which is what made
  teardown-on-deselect tolerable rather than a lurch.

One more behaviour had to be tamed. Stock `juce::WebBrowserComponent` calls
`checkWindowAssociation() → reloadLastURL()` on **every** `visibilityChanged` /
`parentHierarchyChanged` — so a full page reload fired on every Logic
window-switch and every tab re-selection (M3's warm 20 ms is really the cost of
*that* reload against a live view). As of **`9341e98`** this is **suppressed
after the first association while showing**, for the life of each construction:
the initial load renders, every reshow reload after it is dropped, and the lazy
lifecycle is untouched (each new `DashboardWeb` gets a fresh flag). The payoff is
that **deep SPA state and half-typed composer text survive re-shows** — a
Link-window reparent no longer resets the community route to the dashboard home,
scroll position simply stays, and the reload flash is gone.

*Sources: `WEBVIEW_SPIKE_RESULTS.md`, `WEBVIEW_SPIKE_RESULTS_EMBED.md`,
`FINDING_webview-lazy-construction.md`, `FINDING_webview-reloads-on-reshow.md`.*

---

## 3. The bridge contract

**`CONTRACT_dashweb_bridge.md` is normative** — it is what the page is built
against, and its JUCE shapes are quoted from JUCE 8.0.12 source, not recalled.
The essentials:

- **Two native functions, and only two:** `loadChain` and `openChat`. No
  `openProject` / `openBrowser` / `setBadge` yet — an unregistered function is a
  clean feature-detect miss, which is the whole point of registering what exists
  and stubbing nothing.
- **`loadChain({ chainId } | { slug })`** — exactly one key. `slug` for someone
  else's shared chain (the plugin does the share-import itself); `chainId` for
  the user's own. **`openChat({ chatId })`** opens that chat; **`openChat({})`
  or `{ chatId: "" }`** opens the Chat tab with nothing selected (the "See all"
  and null-`latestChatId` fallback — send `{}`, never `{ chatId: null }`).
- **Hard native validation before anything else** (`validateLoadChain`,
  `validateOpenChat` in `Source/DashboardWeb.cpp`; ≤64/≤32 chars, `[A-Za-z0-9_-]`
  only). Malformed → `{ accepted:false, reason:"bad_payload" }` — the page's bug,
  never shown to a user.
- **The answer is an acknowledgement, not a dial report.** On `accepted:true`
  the plugin switches to a native tab, which by the lazy lifecycle **destroys the
  webview** — there is no page left to stream per-slot results to, and the native
  tab shows them anyway. `accepted:true` means "validated and handed off, done on
  my side."
- **One shared busy guard** across both functions: busy if a confirm modal is up
  **OR** a load was accepted within ~8 s (`loadChainBusy`). A `loadChain` in
  flight busies an `openChat` and vice versa — both navigate away and tear the
  webview down. It self-heals; the page must not retry or error-toast a `"busy"`.
- **Per-function feature-detect on the page** via
  `window.__JUCE__.initialisationData.__juce__functions.includes("loadChain")` —
  never UA-sniff, never branch on `?embed=plugin` alone (that controls *layout*;
  the bridge controls *behaviour*).
- **JUCE 8.0.12 wire-protocol pin.** The page replicates the `__juce__invoke` /
  `__juce__complete` protocol rather than vendoring the AGPL
  `juce-framework-frontend`. Those event names are a **private JUCE detail** —
  **re-verify them against JUCE source on any JUCE upgrade**; a silent rename
  there breaks every bridge call with no compile error on either side.

---

## 4. The chain-load path and its protections

**Every** load route — a dashboard row via `loadChain`, a `/chains/mine` row, a
share import, or the native Chain tab — converges on **one loader**:
`openSavedChain` (`PluginEditor.cpp:25447`) → `restoreSavedChain`
(`PluginEditor.cpp:25521`, into `ChainHost`). Slot iteration anywhere else is a
bug; the one-loader rule is the invariant that keeps the four protections in
force no matter how the load was triggered. The bridge's own convergence point is
`bridgeOpenChainById` (`PluginEditor.cpp:10212`), which fetches the name and
calls the two-argument `openSavedChain`.

The four protections, and where each lives:

- **Replace-confirm** (`7b3b456`; guard fix `0e557c1`). A saved chain replacing a
  **non-empty** rack asks first. The flag is cleared **unconditionally** — the
  fix evaluates `std::exchange` first — so a cancelled confirm or a failed
  internal fetch can never leave it stuck silent.
- **Format / uid / version matching, with an apply-time re-check** (`3e7a2e5` +
  `528c6eb`). A saved state chunk is only pushed into a slot if it matches on
  format, uid and version; for a **thin VST3** whose true identity is only known
  at apply time, the match is **re-checked then**. The policy lives in exactly
  one place — `stateFitsPlugin` (`ChainHost.h:988`).
- **The deadman covers state restore** (`a183063`). The crash-guard's phase line
  includes a `state restore` phase (`ChainHost.cpp:4502`), so a plugin that
  crashes *while ingesting restored state* is blacklisted like any other crash
  phase rather than taking the host down on every reload.
- **Per-slot state notes** — each slot records what happened (dialled, or came
  back at defaults and why), which is what the native Chain tab shows in lieu of
  a dial report to the (now destroyed) webview.

One spec correction that matters here: **`state` travels on two single-item
endpoints**, `GET /api/v2/chains/:id` **and** `GET /api/v2/shares/:slug` — the
parity spec's "the only endpoint that returns state" line
(`PLUGIN_DASHBOARD_PARITY_SPEC.md:201,581`) is wrong by one, deliberately, since
a shared chain must carry its state to load.

---

## 5. Dev / preview plumbing

How a dev build reaches the preview dashboard — all of it compiled **out** of
release builds, which talk to production:

- **One host for both.** Dev-transport builds read `~/.echojay/dev.json`, whose
  `baseUrl` is the **same host** for the API and the webview. Never two hosts —
  the whole gate-fallback design assumes the webview origin is the API origin.
- **The v2 gate is a cookie.** The preview host puts `/dashboard` behind an
  `ej_v2_preview` cookie (without it, 404 — a **feature gate, not a missing
  route**). The cookie is seeded by loading `/dashboard?v2preview=<token>` (307 +
  30-day `Set-Cookie`). The token lives in `~/.echojay/v2preview.token`, **never
  in the repo**.
- **The seed never carries `embed=plugin`.** The gate's 307 **strips query
  params**, so a seed URL with `embed=plugin` on it would land on the *full*
  page. The flow seeds the cookie, then loads the embed URL directly once the
  cookie is warm (`DashboardWebFlow`, driven by `tools/dashweb_test`).
- **The webview owns its session.** A persistent `WKWebsiteDataStore`
  (`DashboardWeb.cpp:319`) means you **log in once per machine**; the auth
  session and the gate cookie **survive editor recreation and webview teardown**,
  so a lazy rebuild reloads populated — no re-login, no re-seed.

**Flag:** `V2_PREVIEW_TOKEN` should be **rotated** — it transited a chat upload
on 18 Aug 2026.

**Flag (6 Sep 2026):** the plugin SESSION TOKEN for the test account
`wonderwithsienna@gmail.com` (tier pro, tierLevel 1) was pasted into a chat on
6 Sep 2026 — same class as the line above. **Rotate or invalidate that session
after the shoot** (sign out in the plugin, or invalidate server-side). It
expires on or about **24 Sep 2026**; do not let the expiry be the only thing
that ends it. No token value is recorded anywhere in this repo.

---

## 6. Standing corrections to tribal knowledge

- **Logic does NOT destroy the editor on a Link-window switch.** Measured: CTOR 0
  / DTOR 0 across 20 switches (the DTOR instrumentation proven working — it fires
  on window *close*). **Closing the plugin window** destroys the editor;
  switching to the Link window and back does not. Three places assert otherwise
  (`DashPoll.h:19-21`, `PluginEditor.cpp:429-437`, parity spec §6c), and **§6c's
  route/scroll persistence work is therefore largely unnecessary** for this path.
- **The gate 404 on the preview `/dashboard` is a feature gate**, not a missing
  route. Production `/dashboard` is 200. (One narrow preview-only edge: a handoff
  that mints a single-use 120 s token to `/dashboard` on an *unseeded* preview
  session burns that token onto the gate 404. No caller in the shipped plugin
  today — flagged in `FINDING_dashboard-preview-gate.md` as a backend decision,
  not a surprise.)
- **The native dashboard is display-retired**, not deleted (`8de0217`).
  `ProjectArt.h` still draws, and the whole native `DashboardView` still
  compiles and ships, but the native surface now renders **only** signed-out /
  loading / offline states — never the old cached dashboard (so there is no
  old-dashboard flash before the webview paints). **Full deletion of
  `DashboardTab.cpp` + `tools/dashboard_test` is PENDING explicit product-owner
  approval — do not assume it.**

---

## 7. The saas side, as reported `[saas-side, as reported]`

Everything in this section is quoted as given; I cannot verify it from this repo.

Branch **`feat/dash-next`**. Key commits:

| Commit | What it did |
|---|---|
| `6654df4` | `?embed=plugin` (hero/sidebar hidden pre-paint, visualiser never started); handoff allowlist gains `/chains` + `/feed`; `listLimit` unified to 8 |
| `690e18f` | `dash-bridge.js`; chain rows call the bridge |
| `a796fc9` | `/chains` + `/feed` self-embed via `dash-embed.js` — bridge *presence* decides behaviour; links carry nothing |
| `6266039` | "← Dashboard" back link |
| `916c650` | `openChat` rows |
| `eed0e16` | `/projects` + `/chains/mine` pages; three "see all" hrefs fixed; tiles → latest chat |
| `09742ec` | paging at 48; server-side search on `/chains/mine` |

Deploys via `deploy-preview.sh` to `echojay-dash-preview.vercel.app`. Embed
styling is decided **page-side** by bridge presence **or** the `?embed=plugin`
param.

---

## 8. The merge, when it comes

Direction, per the original plan: bring **`feat/dashboard-tab` INTO
`integration/reasoning-plus-pitch`** (the audio branch keeps its history).
**`MERGE_NOTES.md` is the checklist**, and its **§1 is a silent-merge hazard that
will not show in a diff**:

`integration/reasoning-plus-pitch` widens `openSavedChain` with two **defaulted**
recall callbacks (`onFetchError`, `onSlotsParsed`). A `git merge-tree` dry run
produces **zero conflict markers** — which is the danger. The merge compiles, and
**three call sites on this branch silently bind the two-argument form and drop
those hooks**: the confirm re-entry (`openSavedChain(id, name)` inside the
replace-confirm modal callback), `bridgeOpenChainById` (the loadChain bridge's
convergence point), and the recall hazard §1 already names. Each must gain the
callbacks **by hand** on merge — thread them through, or deliberately pass `{}` —
or recall loses its error reporting and slots-parsed hook with nothing in the
diff to point at it. `MERGE_NOTES.md` §5 also records the small chat-sidebar
scroll additions on this branch (three one-line call sites + one new method), in
case the other machine is editing the same sidebar code.

---

## 9. Remaining work, honestly

- **Messaging shakedown** — reactions, block, and the 5 s fast-poll under load
  are not yet exercised hard.
- **Native-view deletion** — `DashboardTab.cpp` + `tools/dashboard_test` await
  explicit product-owner approval (see §6); the redundant `openDashboardTab`
  fetch into the hidden `dashView_` is left in place for that small-diff reason
  and goes with the deletion.
- **Production rollout** — v2 to prod, the gate-removal path, and a plugin
  **release build against `www.echojay.ai`** (release compiles all preview
  plumbing out).
- **Windows WebView2** — a runtime-present check with a native fallback (route
  like the signed-out case, don't construct into a blank rectangle). Needs a
  Windows machine; `MERGE_NOTES.md` §3. All spike numbers are macOS/WKWebView —
  WebView2 has a different process model; re-verify there before shipping.
- **Token rotation** — `V2_PREVIEW_TOKEN`, per §5.
- **Profile links** — the `/@handle` page isn't embed-aware `[saas-side, as
  reported]`; matters only if the plugin ever links to profiles.

---

*Commit range covered: `7cff677..HEAD` on `feat/dashboard-tab`. The narrative
above compresses ~30 commits — `git log 7cff677..HEAD` is the authoritative
sequence, and the docs named in each section are the primary sources.*
