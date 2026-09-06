# Stage 3 brief: chain load through the bridge

**For a Claude Code session in `~/echojay-vst`, branch `feat/dashboard-tab`
(tip `df8d292` or later).** Read `PLUGIN_DASHBOARD_PARITY_SPEC.md` §2, §5a and
§8, `MERGE_NOTES.md`, and `Source/DashboardWeb.h` before writing anything.

Run the branch guard; report path and branch. Two machines work this repo.

## What stage 3 is

A chain row on the webview dashboard, clicked, loads that chain into the rack
with its saved settings dialled — through the §8 bridge on one side and the
`restoreSavedChain` path hardened in a183063/3e7a2e5/528c6eb/7b3b456 on the
other. Both halves already exist; this stage is the wire between them.

**The one-loader rule (§5a) is the constitution of this stage.** Every route —
dashboard row, own chain, someone else's share — must converge on the
`openSavedChain` path, because that is where the confirm, the format/uid/version
matching, the deadman and the per-slot notes live. A second loader is how one
of them ends up pushing a VST3 chunk into an AU. If you find yourself writing
slot iteration anywhere in this stage, stop.

## The split: plugin half here, page half on the other Mac

The dashboard page is `echojay-saas-dash` code — the other machine's repo. This
stage builds the PLUGIN half and a **written contract** the other Mac builds
the page half against. Neither side guesses at the other.

---

## Part A — the bridge, in `DashboardWeb`

JUCE 8.0.12 native function bindings (confirmed available;
`WebBrowserComponent::Options`). The webview is recreated on every Dashboard
selection, so registration happens at construction inside `DashboardWeb` —
which is fine, and stays inside that class per its own header rule.

Register exactly ONE function this stage: `loadChain`. Do not stub the rest of
the §8 table — an unregistered function is cleanly feature-detectable from the
page; a stub that answers "not implemented" is a liar the page has to special-
case.

- Payload: `{ chainId: string }` or `{ slug: string }` — exactly one of the
  two, both non-empty strings, lengths bounded (id ≤ 64, slug ≤ 32,
  `[A-Za-z0-9_-]` only). **Validated natively, hard, before anything else** —
  the page is our own page and it is still a page (§8). A payload that fails
  validation answers `{ accepted:false, reason:"bad_payload" }` and does
  nothing.
- Expose to the editor as `std::function<void(const juce::String& chainId,
  const juce::String& slug, Answer)> onLoadChain` — DashboardWeb validates and
  forwards; the editor decides.
- **The answer is an acknowledgement, not the dial report.** On success the
  plugin switches to the Chain tab, which destroys this webview (lazy
  lifecycle) — there is no page left to answer per-slot results to, and the
  Chain tab's `setStateNotes` panel already shows them natively. Answer
  `{ accepted:true }` the moment the request is validated and handed off;
  `{ accepted:false, reason }` otherwise. Update the §8 table's "answers
  per-slot dial results" expectation in the contract doc — it predates the
  lazy lifecycle.

## Part B — the editor handler

1. **Idempotency first (§8 rule).** One in-flight guard; a `loadChain` arriving
   while one is pending answers `{ accepted:false, reason:"busy" }`. A
   double-fired click must not build the rack twice — and must not stack two
   confirm dialogs.
2. **Slug route** (someone else's share): `POST /api/v2/shares/:slug/import` →
   `{ chainId }`. Check whether `EchoJayAPI` already has this call (D3.2-era
   import work); add it if not, shaped like the neighbours.
   `{ imported:false, reason:'own_share', chainId }` **is success** — the id
   you already have is exactly what you want next (§5a).
3. **Both routes converge:** with a chainId in hand, go through the
   `openSavedChain` path. It fetches, confirms against a non-empty rack,
   matches, dials, and reports per-slot — all shipped. You will need the
   chain's *name* for the confirm wording; get it the way openSavedChain's
   existing callers do, or fetch-then-open — read the code and follow its
   grain rather than inventing a bypass.
4. On acceptance, switch to the Chain tab (which is where the user watches the
   rack fill in, and which tears down the webview per the lifecycle).
5. **Mind the MERGE_NOTES §1 hazard:** the other branch adds
   `onFetchError`/`onSlotsParsed` params to `openSavedChain`. You are adding a
   third caller — write it so the merge conflict is loud rather than silent,
   and add a line to MERGE_NOTES §1 naming this new call site.

## Part C — the contract document

`CONTRACT_dashweb_bridge.md`, committed to this branch, written for the other
Mac's session to implement the page half against, containing:

- How the page detects the bridge (the exact `window.__JUCE__` shape JUCE
  8.0.12 exposes — read the JUCE source in `~/JUCE` and quote it, do not
  recall it).
- The `loadChain` call signature, payload rules, and every `reason` string.
- The required page behaviour: in embed mode, when the bridge is present,
  chain-row clicks call `loadChain` instead of SPA navigation; when absent
  (ordinary browser), behaviour is unchanged. Feature-detect, never UA-sniff.
- What the page should do with `accepted:false` per reason (`busy`: nothing;
  `bad_payload`: its own bug; etc.).
- The acknowledgement-not-dial-report note from Part A.

## Part D — tests

Extend `tools/dashweb_test` (the flow struct pattern — pure logic, no live
page): payload validation accepts the two legal shapes and rejects garbage —
both keys present, neither present, empty strings, over-length, illegal
characters, wrong types; the in-flight guard answers busy; own_share resolves
to success. Negative control that must fail. `dashboard_test` and
`state_match_test` still pass untouched.

Manual, reported observed-not-assumed (needs the page half live, so it may
wait): click a chain row → confirm appears over a non-empty rack → Chain tab
with the rack filling in and per-slot notes; a share row imports then loads;
double-click does not stack dialogs.

Build per part, commit per part, push when green. PluginEditor.cpp edits stay
inside the Dashboard/chain seams; name exact lines.

## Out of scope

`openChat` / `openProject` (stage 4), `setBadge` (native badge already works),
`openBrowser` (stage 4, with the allowlist re-check), messaging (stage 6),
handoff auto-signin. The page half itself — that goes to the other Mac with
the contract.
