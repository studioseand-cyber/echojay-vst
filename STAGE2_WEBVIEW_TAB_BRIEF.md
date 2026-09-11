# Stage 2 brief: the lazy webview Dashboard tab

**For a Claude Code session in `~/echojay-vst`, branch `feat/dashboard-tab`
(tip `472e5f7` or later).** Read `WEBVIEW_SPIKE_RESULTS.md`,
`WEBVIEW_SPIKE_RESULTS_EMBED.md`, `FINDING_webview-lazy-construction.md`,
`FINDING_webview-reloads-on-reshow.md` and `PLUGIN_DASHBOARD_PARITY_SPEC.md`
§1b/§6/§7/§8/§9 before writing anything. The architecture is decided and
measured; this brief implements it, it does not reopen it.

Run the usual branch guard first. Report path and branch — two machines work
this repo.

## The decided shape

One webview per process, alive only while the Dashboard tab is showing.
Measured costs this design accepts: ~149 MB while visible, ~489 ms cold open,
~20 ms warm re-shows, brief flash on re-show, scroll survives. What it must
never do: exist eagerly (326 MB/instance full-page, ~103 MB/instance embed,
measured), or draw over prompts (native NSView beats all JUCE painting —
FINDING_webview-lazy-construction.md, last section).

**The native `DashboardView` is NOT deleted.** It remains the fallback surface:
signed out, offline / load-failed, and (later) Windows-without-WebView2. The
webview is an upgrade the editor swaps in when it can load; the native tab is
what §7 requires offline — its disk-cached chains list is the only thing a user
can act on with no network.

## Standing context

- The other Mac works `integration/reasoning-plus-pitch`, heavy in
  `PluginEditor.cpp`. Keep editor edits inside the existing Dashboard blocks;
  put everything else in NEW files. Name exact line ranges you touch.
- `JUCE_WEB_BROWSER=0` in CMakeLists.txt today. Flipping it to 1 and linking
  WebKit is part of this work (the spike proved it builds; the spike branches
  are deleted — do not go looking for them, the results docs carry everything).
- Push when green. Never force. Do not commit any token or secret; the token
  file stays at `~/.echojay/v2preview.token`.

---

## Part A — `Source/DashboardWeb.h/.cpp` (new files)

A component owning one `juce::WebBrowserComponent` and its lifecycle. The
editor talks to this class only; JUCE webview quirks stay inside it.

1. **URL building.** The webview host is THE SAME base the plugin's API uses —
   `dev.json`'s `baseUrl` when the dev transport is active, production
   otherwise. Never a second hardcoded host: the webview and the API must not
   point at different servers (that mismatch nearly happened once already).
   Path: `/dashboard?embed=plugin`.
2. **Preview-gate scaffolding** (dev-transport builds only, compiled out
   otherwise). The gate 307 STRIPS query params, so never put `embed=plugin`
   on a seed URL. Load `/dashboard?embed=plugin` directly; only if the page
   comes back as the gate 404 and `~/.echojay/v2preview.token` exists, load
   `/dashboard?v2preview=<token>` once, then navigate to the embed URL. Log
   each step via `ejDashLog`, token value never logged.
3. **Load-outcome signal.** `std::function<void(bool ok)> onLoadResult` —
   fired from `pageFinishedLoading` / `pageLoadHadNetworkError`. The editor
   uses it to fall back to the native view. A gate 404 after the seed attempt
   counts as failure.
4. **Auth is the page's own session.** The persistent WKWebsiteDataStore holds
   it (measured: survives editor recreation). If the page shows its login
   form, that is acceptable for this stage — the user signs in once per
   machine. `mintHandoff` auto-signin is a later slice; leave a TODO naming
   EchoJayAPI.h:890.

## Part B — editor integration

All inside the existing Dashboard blocks in `PluginEditor.cpp`/.h.

1. **Lazy lifecycle.** Construct `DashboardWeb` when the Dashboard tab becomes
   current AND the editor is on Screen::Main AND no prompt/overlay is up;
   destroy it when the tab stops being current (tab switch, screen change,
   editor destruction). Destroy means destroy — the component, not
   setVisible(false); the measured 103 MB/instance is resident processes, and
   hiding does not free them.
2. **Visibility.** Bounds and visibility authored unconditionally in resized(),
   the tests INSIDE the one visibility expression — the same block, and the
   same five prompt/overlay guards, that 0de7629 just extended for the native
   view. A prompt appearing while the webview shows must hide it (and given
   reload-on-reshow, re-showing costs ~20 ms warm; acceptable, measured).
3. **Fallback wiring.** On construct, show native `dashView_` until
   `onLoadResult(true)`; on `onLoadResult(false)`, keep native and do not
   retry until the next tab selection. Signed out (`!api.isLoggedIn()`) skips
   the webview entirely — the native signed-out line already handles it.
4. **The badge stays native.** DashPoll and the tab-strip dot are untouched.

## Part C — keyboard, verified not assumed

The editor's `keyPressed` (PluginEditor.cpp:27222) intercepts space for
transport/capture when no named TextEditor has focus. A WKWebView is a native
view: determine what actually happens when the user types in the page (the
login form is in scope this stage) — does the editor's keyPressed fire, does
space reach the page or the transport? Report what you find; if space triggers
transport while a webview input is focused, gate the space branch on the
webview having keyboard focus. Do not build the §8 focusChanged bridge for
this — that arrives with messaging.

## Part D — verification

`tools/dashboard_test` must still pass untouched (the native view is shipped
code). Add `tools/dashweb_test` where logic is testable without a live page:
URL building from a fake base, gate-fallback state machine (404 → seed → embed
→ second 404 = fail), the never-seed-with-embed-param rule, and a negative
control that must fail.

Manual matrix to hand back to me, each line observed not assumed:
- fresh instance: prompts show ABOVE everything, dismissible; Dashboard shows
  native until webview loads, then swaps
- tab away → memory command shows WebContent/GPU gone within ~a minute; tab
  back → ~0.5 s rebuild, populated, no re-login
- token file absent (rename it): gate 404 → native fallback, loud log line
- Logic Link-window switch with Dashboard open: no editor recreation expected
  (measured), webview intact
- close/reopen plugin window: webview rebuilt, session survives
- typing in the webview login form: space is a space, transport unaffected

Build after each part; commit per part; push when all green.

## Out of scope, deliberately

The §8 bridge calls (loadChain/openChat/openBrowser/setBadge) — stages 3–4.
Messaging and its keyboard work — stage 6. Handoff auto-signin. Windows
WebView2 runtime detection (needs a Windows machine; note it in MERGE_NOTES).
Deleting the native view — it is the offline story, keep it healthy.
