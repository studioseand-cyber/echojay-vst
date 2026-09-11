# Brief: write the Dashboard handover document

**For a Claude Code session in `~/echojay-vst`, branch `feat/dashboard-tab`.**
Output: one file, `HANDOVER_DASHBOARD.md`, committed to this branch and pushed.
It replaces this conversation as the place where the whole system exists as one
story. Write it for a reader with neither this chat nor either session's
history — you in three months, or the other machine's session tomorrow.

Assemble it from the documents already on this branch — the findings, the two
spike results docs, MERGE_NOTES, CONTRACT_dashweb_bridge.md, the parity spec —
plus `git log 7cff677..HEAD`. Where this brief states cross-repo facts you
cannot verify from here, quote them as given and mark them `[saas-side, as
reported]`.

## What it must contain, in this order

### 1. What shipped, in one paragraph
The plugin's Dashboard tab is the real v2 web dashboard in a lazy WKWebView,
with a native bridge for the three things only the plugin can do: load a chain
into the rack with its settings dialled, open a chat in the plugin's Chat tab,
and light the tab badge. Messaging, community, feed, search and every future
web change arrive with no plugin work. State the date range (18–19 Aug 2026)
and the two repos.

### 2. The architecture, and the measurements that chose it
Lazy single-instance webview: constructed on Dashboard-tab selection, destroyed
on deselection, never more than one per process, hidden by the same visibility
expression that guards the intake prompts (a native NSView cannot be
out-z-ordered). The numbers, from the two results docs: full page 326 MB /
1,273 MB (fail), embed 149 MB / 869 MB (fail at 8 eager — hence lazy), warm
re-show 20 ms, cold rebuild 489 ms, thresholds pre-registered. Name the two
levers that made it viable: `?embed=plugin` (static background, no visualiser)
and lazy+teardown. Note reload-on-reshow is SUPPRESSED after first association
(9341e98) — deep SPA state and typing survive re-shows.

### 3. The bridge contract
Point at CONTRACT_dashweb_bridge.md as normative. Summarise: two functions,
loadChain ({chainId}|{slug}) and openChat ({chatId} or EMPTY = Chat tab
unselected); hard native validation; acknowledgement-not-dial-report; the
shared busy guard (modal check + ~8s timestamp); per-function feature-detect on
the page; the JUCE 8.0.12 wire-protocol pin (page replicates
__juce__invoke/__juce__complete rather than vendoring the AGPL frontend lib —
re-verify on any JUCE upgrade).

### 4. The chain-load path and its protections
Every load route (dashboard row, /chains/mine row, share import, Chain tab)
converges on openSavedChain → restoreSavedChain. The four protections and
where they live: replace-confirm (7b3b456, guard fix 0e557c1),
format/uid/version matching with apply-time re-check for thin VST3s
(3e7a2e5 + 528c6eb, policy in stateFitsPlugin, ONE place), the deadman with the
`state restore` phase (a183063), per-slot state notes. `state` travels only on
the two single-item endpoints (chains/:id and shares/:slug — the spec's
"only endpoint" line is wrong by one). One-loader rule: slot iteration
anywhere else is a bug.

### 5. Dev/preview plumbing
How a dev build reaches the preview: dev-transport builds read
~/.echojay/dev.json (baseUrl = the SAME host for API and webview — never two);
the v2 gate is the ej_v2_preview cookie, seeded via
/dashboard?v2preview=<token> where the token lives in ~/.echojay/v2preview.token
(never in the repo; the 307 STRIPS query params so a seed URL never carries
embed=plugin); the webview's session is its own persistent WKWebsiteDataStore
(login once per machine; survives editor recreation). Release builds compile
all of this out and talk to production. Flag: V2_PREVIEW_TOKEN should be
rotated (it transited a chat on 18 Aug).

### 6. Standing corrections to tribal knowledge
- Logic does NOT destroy the editor on a Link-window switch (measured: CTOR/
  DTOR 0 over 20). Closing the plugin window does. Three places asserted
  otherwise; §6c's persistence work is unnecessary.
- The gate 404 on preview /dashboard is a feature gate, not a missing route.
- ProjectArt.h draws and is now display-retired along with the whole native
  DashboardView (8de0217): native shows only signed-out / loading / offline.
  Full deletion of DashboardTab.cpp + dashboard_test is PENDING explicit
  product-owner approval — do not assume it.

### 7. The saas side, as reported [saas-side, as reported]
Branch feat/dash-next. Key commits: 6654df4 (?embed=plugin, allowlist +/chains
+/feed, listLimit 8 unified), 690e18f (dash-bridge.js, chain rows), a796fc9
(/chains + /feed self-embed via dash-embed.js — bridge presence decides, links
carry nothing), 6266039 (← Dashboard back link), 916c650 (openChat rows),
eed0e16 (/projects + /chains/mine pages, three see-all hrefs fixed, tiles →
latest chat), 09742ec (paging at 48, server-side search on /chains/mine).
Deploys via deploy-preview.sh to echojay-dash-preview.vercel.app. The embed
styling is decided page-side by bridge presence OR ?embed=plugin param.

### 8. The merge, when it comes
Direction per the original plan: bring feat/dashboard-tab INTO
integration/reasoning-plus-pitch (the audio branch keeps its history).
MERGE_NOTES.md is the checklist — especially §1: openSavedChain gains
onFetchError/onSlotsParsed on the other branch, merges conflict-FREE, and
three call sites here (confirm re-entry, bridgeOpenChainById, and the recall
hazard) must gain the hooks by hand or recall loses its reporting silently.

### 9. Remaining work, honestly
Messaging shakedown (reactions, block, 5s fast-poll under load); native-view
deletion pending approval; production rollout (v2 to prod, gate removal path,
plugin release build against www.echojay.ai); Windows WebView2 runtime check +
fallback (MERGE_NOTES §3, needs a Windows machine); token rotation; the
/@handle page isn't embed-aware (flagged saas-side) if the plugin ever links to
profiles.

## Rules
Prose, not bullet-soup, except where a table genuinely compresses (the
measurements, the commit map). Every claim either verifiable from this repo or
marked as reported. No secrets — token paths yes, token values never. Keep it
under ~400 lines. Commit as docs, push, and report the file path and commit.
