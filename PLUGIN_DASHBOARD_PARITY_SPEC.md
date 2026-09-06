# EchoJay v2 — the plugin Dashboard tab, at parity

Status: NOT STARTED. Written 17 Aug 2026.
Plugin repo: `~/Documents/ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200`
Backend repo: `echojay-saas-dash`, branch `feat/dash-next`.

**SUPERSEDES `SESSION_C_BUILD_SPEC.md`**, which described the same tab as a thin
read-only mirror. Read that document first anyway: sections 1 (prerequisites), 4
(the handoff URL), 5 (polling and the Logic constraint) and 6 (offline) are
unchanged and are not repeated here in full.

---

# 0. What changed, and what it costs

Session C's rule was:

> **Thin, not a mirror.** Read and navigate only. No text entry anywhere on this
> tab. Anything editorial, social or requiring typing opens the browser.

**Reversed by the product owner on 17 Aug 2026.** The ask, in his words: everything
on the dashboard exactly the same on the plugin Dashboard tab apart from the hero,
send and receive messages the same way so you can use the community in the plugin,
chains loadable into the chain list, "view all" links opening the website, chat links
opening the right chat in the plugin, and all of it reactive.

**The cost of reversing it, stated so nobody rediscovers it in three months.**

1. Session C's rule was not squeamishness. It bought two things: a small surface
   with no keyboard handling, and a second renderer that could not drift far from
   the first because it did so little. Both are now spent.
2. Text entry inside a plugin editor is a **host keyboard problem**, not a UI
   problem. See section 4. It is the single highest-risk item in this document and
   it is the reason the original rule existed.
3. Anything drawn twice must be changed twice. Section 1 is entirely about not
   paying that.

**What did NOT change:** the umbrella spec's "one payload, two renderers" claim, the
handoff allowlist discipline, the Logic editor-destruction rule, and the offline
cache pattern from Session B. Those survive this reversal intact.

---

# 1. THE ONE DECISION THAT GATES EVERYTHING ELSE

The plugin is native JUCE C++. "Exactly the same as the web dashboard" is therefore
two completely different projects, and nothing below can be estimated until this is
decided.

## 1a. Native reimplementation

Rebuild the web dashboard in C++.

What that actually means, measured rather than guessed:

| File | Size |
|---|---|
| `public/js/dashboard.js` | ~2,900 lines |
| `public/js/community.js` | ~3,100 lines |
| `public/js/feed-rows.js` | ~800 lines |
| `public/js/chain-rows.js`, `nav.js`, `account-card.js`, `workspace-tree.js` | ~1,400 lines |
| `public/css/dashboard.css` | 73 KB |

Plus, for parity: three editorial columns, the community panel with ten tabs, two
message trays, reactions, followers/following with server-side search, the feed with
generated art, project tiles, chain rows with five share controls, notifications.

**And every future web change lands twice, forever.** The two surfaces have already
drifted once in a way that cost a day: `+ New Chat` was a bordered cyan control in
the plugin's own sidebar and a grey text row everywhere else, because the styling
lived in three files the other five surfaces do not load. That was ONE control.

**In favour:** no runtime dependency, full control of keyboard focus, works offline
against Session B's disk cache, no memory cost beyond the components.

## 1b. WebView

`juce::WebBrowserComponent` loading `/dashboard?embed=plugin`.

**In favour, and it is decisive on the stated goal:** it *is* the same code. Parity is
not a target to hit and re-hit, it is a property. Messaging, community, feed, search
and every future addition arrive with no plugin work at all.

**Against, and every one of these needs a designed answer, not a hope:**

1. **Windows.** WebView2 requires the Evergreen Runtime. Pre-installed on Windows 11
   and on most Windows 10 since 2021, **not guaranteed**. Needs a presence check at
   editor construction and a designed fallback — the native-lite tab of section 3d,
   or an honest "open in browser" panel. Never a blank rectangle.
2. **Logic destroys the editor** on every switch between the Link window and EchoJay.
   The webview is destroyed and reloaded with it: white flash, lost scroll position,
   lost open thread, lost half-typed message. This is the same class as Session C's
   rule that the poll timer lives on the processor. Mitigation in section 6c.
3. **N instances = N webviews.** A session with eight EchoJay instances would run
   eight copies of the dashboard, each polling. Gating in section 6d.
4. **Offline.** A webview cannot render Session B's disk cache. Section 7.
5. **Memory.** One WKWebView/WebView2 per editor is tens of MB. Measure it on the
   heaviest realistic session before committing.
6. **Token injection** without putting a JWT in a URL. Section 5c.
7. **JUCE version.** Native function bindings (`WebBrowserComponent::Options`) are
   JUCE 8. On JUCE 7 the bridge is URL-scheme interception via `pageAboutToLoad`.
   **Confirm which JUCE the plugin is on before designing section 8.**

## 1c. Recommendation

**WebView for the dashboard body, native for the three actions that touch plugin
state, native for the tab badge.** Reasons, in order:

- The stated goal is parity. 1b gives it as a property; 1a gives it as an ongoing
  bill, and the `+ New Chat` incident is evidence about how that bill gets paid.
- The three things that genuinely need to be native — load a chain into the rack,
  open a chat, light the tab badge — are a small, well-defined bridge, not a
  renderer.
- Messaging is the largest single chunk of 1a and the least valuable to rewrite,
  because it is pure forms and lists.

## 1d. Survey before writing any code

In the plugin repo, answer and report:

1. JUCE version, and whether `WebBrowserComponent` native function bindings are
   available.
2. Does a `WebBrowserComponent` inside the editor survive a Logic Link-window switch,
   or is it reconstructed? Measure, do not assume.
3. Memory delta per instance with the webview up, on the heaviest session you have.
4. WebView2 runtime presence check: what does the plugin currently do on a Windows
   machine without it?
5. Does the tab strip have room? Session C section 2 is unanswered: you are at seven
   tabs, this makes eight, and compact mode in a narrow DAW window needs a designed
   answer (icon-only, scroll, or overflow).
6. Keyboard: what does the editor currently do with key events, and does any existing
   field take focus? Section 4a depends on the answer.

---

# 2. CHAIN PARAMETERS — the vital one

> "saved chains need their parameters this is essential and when loaded into EchoJay
> it needs to dial them this is vital"

**The good news: this is already built end to end in the schema and the API.** Nothing
below is new backend work. What is missing is the plugin doing it, plus a matching
policy and honest UI.

## 2a. There are TWO parameter mechanisms. Do not confuse them.

### `state` — the real one, and the one that dials a plugin exactly

`dash.chains.state` is **an object keyed by the 1-BASED slot number, values base64 or
null**. Each value is the plugin's own opaque `getStateInformation` blob.

```json
{ "1": "VkMyIQAAA...", "2": null, "3": "eNqFkE1P..." }
```

- **Keyed, not positional, deliberately**: `null` means "did not capture", never
  "captured nothing". A slot missing from the object is the same as null.
- Caps, from `lib/dash/chains.js`: **256 KB per slot, 1 MB per chain**, measured on
  the DECODED bytes because that is the unit `getStateInformation` works in. Capping
  the encoded string would reject at ~192 KB of real state and the two halves would
  disagree — "the kind of mismatch that only shows up on somebody's Kontakt
  instance", as the file already says.
- The server **rejects rather than truncates**. Silently dropping half a blob would
  produce a chain that loads into a wrong-sounding rack.
- Validation is `validateState(state, slotCount)`, and every rejection names the slot
  and the numbers: `bad_state`, `bad_state_key`, `state_slot_out_of_range`,
  `bad_state_value`, `state_not_base64`, `state_slot_too_large`.

**This is what "dial them" means.** `setStateInformation` restores every parameter a
plugin has, including ones nobody modelled, in the plugin's own format.

### `slots[].params` — a typed vocabulary, and NOTHING WRITES IT

`normalizeSlots` accepts a `params` object per slot, validated against
`lib/validate-settings.js` — a **closed, fully typed 18-key vocabulary**, the same
validator that guards the chat path. Unknown keys and out-of-range values are
DROPPED, never stored.

**But: no writer exists.** The web save does not send params, and the plugin cannot,
because `settings_structured` is produced server-side inside a chat turn while a save
is initiated by C++ holding its own idea of the rack. The schema went first on
purpose so a future writer meets a column that already expects values.

**Do not build this tab against `params`.** It is always null today. Use `state`.
If a writer is ever wanted, that is its own slice.

## 2b. The pipeline that already exists

```
CAPTURE      plugin: getStateInformation per slot, base64, cap at 256KB/slot
   ↓         degrade honestly: null the slots you could not capture
SAVE         POST /api/v2/chains         { name, slots, state }
   or        PATCH /api/v2/chains/:id    overwrite, no cap charged
   ↓         validateState re-checks server side; rejects, never truncates
SHARE        the snapshot FREEZES state at share time:
   ↓         jsonb_build_object('name', c.name, 'slots', c.slots,
             'slotsVersion', c.slots_version, 'state', c.state, 'frozenAt', now())
IMPORT       POST /api/v2/shares/:slug/import
   ↓         importShare copies slots, slots_version, state, art_seed
             (NOT project_id, NOT favourite — see the note in shares.js)
LOAD         GET /api/v2/chains/:id  ← THE ONLY ENDPOINT THAT RETURNS state
   ↓         one chain at a time, ownership in the WHERE clause
DIAL         plugin: instantiate per slot, then setStateInformation per 1-based key
```

**`state` NEVER travels in a list.** Not in the dashboard payload, not in a share
listing, not in a message attachment — `publicAttachment` strips it explicitly. The
payload's chains carry `hasState: boolean` and nothing more. **The plugin dashboard
must respect this**: the tab shows `hasState`, and fetches `state` only on a
deliberate load. A dashboard that pulled state for twelve rows would move a megabyte
per row.

## 2c. Format and version matching — the part that is NOT built, and matters

`state` is opaque and **format-bound**. A VST3 chunk will not load into the AU build
of the same plugin, and will not reliably load into a different version. This is
exactly why `normalizeSlots` carries `manufacturer`, `plugin`, `format`, `version`
and `uid` per slot.

**The rule, and it must be implemented in the plugin:**

| Match | Action |
|---|---|
| `uid` **and** `format` both match | Instantiate and `setStateInformation`. Full dial. |
| plugin found, `format` differs | Instantiate. **DO NOT push the chunk.** Report per slot. |
| plugin found, `version` differs, same format+uid | Attempt the chunk, but see below. |
| plugin not found | Empty slot, named, reported. Never a silent gap. |

**A chunk pushed into the wrong binary is worse than no chunk.** Best case it is
ignored; realistic case the plugin mis-parses and lands on garbage parameters that
sound wrong and look deliberate; worst case it crashes the host — which is precisely
what the deadman marker in `ChainHost` now records under the `state restore` phase.
Do not treat "it usually works" as the policy.

**Version mismatch is a judgement call and needs your decision.** Most plugins
version their own chunks and tolerate an older one. Some do not. The safe default is
**attempt it, with the deadman covering the call**, because the deadman now
blacklists a plugin that dies during state restore — so the failure mode is one bad
plugin recorded and withheld, not a repeatedly unloadable session. State that.

**The `vst3InAuHostExperiment` flag is directly relevant.** A chain captured from the
VST3 build hosted inside the AU host records `format: "VST3"`. With the flag off, the
same rack loads the AU build and the chunk must be withheld. The chain's slots know
which build was captured; use it.

## 2d. What the UI must say, honestly

Three states, three different sentences. This is the difference between a feature
people trust and one they stop using.

1. **`hasState` true, every slot matched** → "Loads with all settings."
2. **`hasState` true, some slots matched** → load, then report which slots did not
   dial and why: plugin missing, or format differs. Per slot, named. Not a count.
3. **`hasState` false** → "Loads the rack, not the settings." A chain saved before
   capture existed, or one whose slots all blew the cap, is still a useful chain and
   must not be presented as a broken one.

**Never claim a dial you did not perform.** A rack that loaded at plugin defaults
while the UI said "loaded with settings" is the single fastest way to lose trust in
saved chains.

## 2e. Save from the plugin dashboard?

**No.** Capture and save belong to the Chain tab, which owns the rack. The dashboard
tab lists chains and loads them. One writer, one place. Loading is the action this
tab adds.

---

# 3. Surface parity — what appears on the tab

The web dashboard, top to bottom, and its disposition here.

| Web dashboard band | Plugin Dashboard tab |
|---|---|
| **Hero** (drop a song, ask a question) | **REMOVED** — product owner, and correct: the plugin already has Chat and the file is already in the DAW. |
| Onboarding checklist (`payload.onboarding`, while incomplete) | Same. Read-only, no dismiss — it disappears on completion. |
| Feature cards (Oracle / Prompt mixing / Content creators) | Same. Each opens the browser. |
| Editorial row: **Trending chains**, **Recently shared**, **Featured** | Same, with generated art. Rows load into the rack (section 5a). "View all" opens the browser (5b). |
| **Community panel** (10 tabs: Announcements, Messages, Feed, Followers, Following, Find, Notifications, Invite, + Reports/Featured for admins) | Same. Section 4. |
| **Your work**: Projects rail, Your chains, Recent chats | Same. Projects and chats deep-link into the plugin (5c). Chains load into the rack (5a). |
| Sidebar: nav, account card, usage bar, workspace tree | **The plugin already has these.** Do not draw a second sidebar inside the tab — `?embed=plugin` removes it (section 9a). |

**Art.** `ProjectArt.h/.cpp` is ported and validated against a 505-seed fixture and
has never drawn a tile. Chain art is a second generator (`art-core.js`,
`renderChainArtSvg`) — under 1b the webview draws both and the C++ port is only
needed if 1a is chosen. **If 1b: `ProjectArt` stays unused, and that is a real cost
of 1b worth naming.**

---

# 4. Messaging in the plugin

## 4a. The keyboard problem — read this before designing anything

In Logic, Live, Pro Tools and Cubase, key events belong to the **host** unless the
plugin takes focus. Two failure modes, both notorious:

1. **The plugin does not take focus** → you type and nothing appears, or worse, `s`
   solos a track and `space` starts the transport.
2. **The plugin takes focus and does not give it back** → the user cannot control the
   transport without clicking away, and `space` inside a message box is a space
   rather than play/stop, which is right for the box and infuriating everywhere else.

**Neither is acceptable as a default.** The design that works:

- A text field takes keyboard focus **only on an explicit click into it**, never on
  tab open, never on panel switch, never on mount.
- **Escape** and a visible close/blur affordance both release focus. Escape is the
  reflex; the affordance is for people who do not know the reflex.
- While a field has focus, the plugin swallows keys — including space — and this must
  be **visibly obvious**: a focus ring plus a hint that Escape returns control to the
  DAW.
- On focus loss, keys return to the host immediately.
- Under 1b: the webview swallows keys when an input is focused. The same rules apply
  and are harder to enforce, because the focus lives inside a browser. **This needs
  the section 1d item 6 survey answer before it can be specified further.**

**Verify on every host you ship on.** Logic, Live, Pro Tools, Cubase, Reaper,
Studio One. A field that eats the transport in one of them is a support burden that
outlives the feature.

## 4b. What messaging must do

Everything the web panel does, because that is the ask:

- Two trays: **Messages** (people you follow) and **Message requests** (everyone
  else), split server-side by `tray` on each thread from `GET /api/v2/dm`.
- Read a thread: `GET /api/v2/dm/:id` — returns messages, `reactions`, and
  `conversation.canReply`.
- Send: `POST /api/v2/dm` with `{ handle }` or `{ connectionId }`, or
  `POST /api/v2/dm/:id`. Never a raw uid — that path was removed deliberately.
- Reactions: `POST`/`DELETE /api/v2/community/messages/:id/reactions`, fixed
  five-emoji set validated server side.
- Announcements channel: `GET /api/v2/community/conversations/:id/messages`.
- Block: `POST /api/v2/blocks`. **Keep it prominent** — it is the only escape hatch
  since 0028 made anyone able to message anyone.
- Followers / Following: `GET /api/v2/follows?list=followers|following&q=`, searchable.
- Find people: `GET /api/v2/people?q=` — **rate limited**, 30/min and 120/hour, the
  only rate-limited read in the product. Debounce at 350ms and handle
  `{ error: 'rate' }` as a named state, not an empty list.

## 4c. Attachments

A chain attached in a message uses `POST /api/v2/dm/attach`, and opening one is
`openAttachment` — the deliberate one-at-a-time fetch that DOES return state. **This
is the same load path as section 2b and must reuse it, not a second copy.** In the
plugin, "Open" on a chain attachment should offer to load it into the rack, which is
strictly better than the web's "add a copy".

---

# 5. Navigation — the three destinations

## 5a. Chain → the rack, and into the Chain tab

The headline plugin-native action. Two cases:

**Somebody else's shared chain** (Trending, Recently shared, Featured, feed rows,
message attachments):

```
POST /api/v2/shares/:slug/import   → { chainId }   copies slots+state
GET  /api/v2/chains/:chainId       → full chain INCLUDING state
→ ChainHost: instantiate per slot, then setStateInformation per 1-based key
→ switch to the Chain tab with the new chain selected
```

`importShare` on your own share is **not an error**: it answers
`{ imported: false, reason: 'own_share', chainId }` — the id you already have, which
is exactly what the client wants next. Handle it as success.

**Your own chain** (Your chains): skip the import, `GET /api/v2/chains/:id` directly.

**Both paths converge on one loader.** One function that takes a chain-with-state and
dials the rack, used by the dashboard tab, the Chain tab and the message attachment.
Three copies of the matching policy in section 2c is how one of them ends up pushing
a VST3 chunk into an AU.

**Load is destructive to the current rack.** Confirm before replacing a rack with
unsaved changes, the same way the Chain tab does today. If the Chain tab does not do
this today, that is a bug to fix there first.

## 5b. "View all" → the website

Already built: `POST /api/v2/handoff { to }` → `{ url }`, open it, `/go#t=<token>`
redeems a single-use 120-second token and puts the real JWT in the response BODY.
Fragment, never a query string, so it reaches no request log and no `Referer`.

**The allowlist is four literals** and is the thing that stops this being an open
redirect firing with a fresh session attached:

```
/app
/app?overlay=community
/dashboard
/settings/profile
```

**Backend work needed** (section 9b): add `/chains` and `/feed` as literals. They are
one line each.

**`/c/:slug` must NOT be added**, and the reason is already written in
`lib/dash/handoff.js`: that page is public and unauthenticated, so a handoff would
attach a credential to a page that does not need one. **Open `/c/<slug>` directly,
with no handoff.** Same for `/@handle`.

**Never teach `isAllowedPath` to parse.** Whole literals only. That rule is what makes
`/app?overlay=community&next=//evil.com` simply a different string rather than a
bypass.

## 5c. Chats and projects → inside the plugin

Already the payload's `DeepLink` design:

- `recentChats[].id` → switch to the Chat tab, select that chat.
- `projects[].latestChatId` → the same. Added to the payload on 17 Aug 2026, computed
  from the workspace rollup the build already walks, so it cost no query. **Null means
  the project has no chat with any messages** — fall back to the Chat tab with nothing
  selected, never to an empty chat.

Under 1b these arrive over the bridge (section 8).

---

# 6. Reactive

## 6a. Two polls, two costs — unchanged from Session C

| | Endpoint | Interval | Cost |
|---|---|---|---|
| Unread badge | `/api/v2/community/poll` | 20s while the editor is open | 1 Redis round trip |
| Full payload | `/api/v2/dashboard?surface=plugin` | on tab open, and its 60s TTL | 4 Redis, 4 Postgres cold |

**Do not refetch the payload every 20 seconds.** It carries `community.unread`, which
makes it tempting, and it is ~100× the poll. The poll's `rev` is monotonic: if it has
not moved, do nothing at all.

## 6b. Messaging needs a faster tier

The web client already does this: `POLL_MS` 20s, `FAST_POLL_MS` **5s while a DM
thread is open on a visible tab**. Mirror it exactly, and mirror the conditions —
open thread AND visible — or eight instances idling in a closed DAW window will each
poll every five seconds forever.

## 6c. The Logic constraint — the standing gotcha

**The poll timer and the unread count live on the processor, never the editor.** Logic
recreates the editor on every switch between the Link window and EchoJay. Anything on
the editor is destroyed on that switch.

- Poll off the message thread; marshal back with `MessageManager::callAsync` and a
  `SafePointer`.
- The editor is a listener; it does no network work of its own.
- Session B's chain sidebar already solved this shape. Follow it.

**Under 1b, add: the webview's route, scroll position and any half-typed message must
be persisted on the processor and restored on editor construction.** Otherwise every
Link-window switch throws away what the user was doing. This is item 2 of section 1b
and the main reason 1d item 2 must be measured before committing.

## 6d. Multiple instances

Eight instances must not mean eight dashboards polling.

**Rule: only the instance whose editor is open AND focused polls at the fast tier.
Everything else is idle.** The unread count lives on the processor per instance, which
means eight processors each holding a copy of the same global number — acceptable, but
the poll must not be eight-fold. Prefer a single shared poller if the plugin has any
cross-instance singleton already; if it does not, this is not worth inventing one for,
so gate on focus.

---

# 7. Offline

Session B built a disk cache for the chains list, with a "Last updated 14:32" label
and a clear distinction between "no connection" and "this no longer exists". **Reuse
it. Do not invent a second pattern.**

- Cache the last successful payload per account. Render immediately on tab open,
  refresh behind it.
- A failed refresh keeps the cached view **labelled with its time**, never replaced by
  an error.

**Under 1b this does not work for the webview body**, and that is a real regression
against Session C. The honest answer: cache and render natively the parts that matter
offline — the chains list, which is the only thing you can act on with no network —
and show a plain "no connection" panel where the webview would be. A community feed
is useless offline anyway; a chain you can load is not.

---

# 8. The bridge — only under 1b

A narrow, explicit contract. JS in the page calls native; native answers.

**Transport:** JUCE 8 native function bindings if available (1d item 1), else
`pageAboutToLoad` interception of an `echojay://` scheme.

**Calls, and this is the whole surface:**

| Call | Payload | Native does |
|---|---|---|
| `loadChain` | `{ slug }` or `{ chainId }` | Section 5a, end to end. Answers per-slot dial results. |
| `openChat` | `{ chatId }` | Switch to Chat tab, select. |
| `openProject` | `{ projectId, latestChatId }` | Switch to Chat tab, select `latestChatId`, or Chat with nothing selected. |
| `openBrowser` | `{ to }` | Mint a handoff, open the URL. **Native validates `to` against the same allowlist** — never trust the page. |
| `setBadge` | `{ count }` | Tab label dot. |
| `focusChanged` | `{ hasTextFocus }` | Section 4a: native knows whether to swallow keys. |

**Native → page:** `visibilitychange`-style notification so the page can pause its own
polling when the tab is not showing, and a `token` injection at load (section 9c).

**Rules:**

- **The page is not trusted.** It is our own page, and it is still a page: every
  call's parameters are validated natively. `openBrowser` in particular re-checks the
  allowlist, or the bridge becomes the open redirect the allowlist exists to prevent.
- **No `evalJavaScript` of page-supplied strings**, in either direction.
- Every call is **idempotent or explicitly confirmed**. A double-fired `loadChain`
  must not build the rack twice.

---

# 9. Backend work, in `echojay-saas-dash`

All small. All independent of the 1a/1b decision except 9a and 9c.

## 9a. `?embed=plugin` on `/dashboard`

Hides the hero and the sidebar shell; keeps every band below. One query param, read
in `dashboard.js` and `dashboard.html`, adding a body class. **Under 1a this is still
useful** as the reference rendering to build against and to screenshot-diff.

Must also: suppress the page field / visualiser (the plugin has its own), and suppress
the `?v=` cache-key problem by being a class, not a second stylesheet.

## 9b. Two handoff allowlist entries

`/chains` and `/feed`, as whole literals, in `ALLOWED_PATHS`. Update
`allowedPaths()`'s consumers and the error body. **Do not** add `/c/:slug`.

## 9c. Token handoff into the webview

The plugin holds a bearer token. The page needs it in `localStorage['ej-token']` —
the same key OAuth and password login write.

Two options, in order of preference:

1. **Native injection after load.** The bridge sets it before the page's first fetch.
   Cleanest: the token never appears in a URL at all. Needs a load-order guarantee.
2. **Reuse the existing handoff.** Load `/go#t=<token>&to=/dashboard?embed=plugin`.
   Requires `/dashboard?embed=plugin` as a new allowlist literal, and the fragment
   must survive the webview's own URL handling — which is Session C section 1 item 5
   again, and must be verified, not assumed.

**Never a query string.** That is the decision this repo has already made twice, at
`api/oauth/google/callback.js` and in `lib/dash/handoff.js`.

## 9d. `surface=plugin` list limits

`listLimit` is 5 for the plugin against 8 for the web. **At parity that is now wrong**
— the tab is not a summary any more. Either raise it to 8 or drop the distinction.
Raising it is one line; dropping it removes a branch. **Decide, do not leave it at 5
and call it parity.**

---

# 10. Verification

Follow this repo's discipline: every suite carries a **negative control that must
fail**, and a suite that passes with zero failures has proven nothing.

## Backend, in `echojay-saas-dash`

- `?embed=plugin` hides the hero and sidebar and nothing else — asserted structurally,
  not by a screenshot.
- The allowlist accepts `/chains` and `/feed`, rejects `/c/anything`, and rejects
  `/chains?next=//evil.com`.
- `GET /api/v2/chains/:id` remains the only endpoint returning `state`. **This already
  has a guard-shaped assertion available: scan every route and lib for `state` in a
  list response.** Worth adding, given section 2b depends on it.

## Plugin

- **State round trip, byte-exact.** Capture a rack, save, share, import to a second
  account, load, and compare `getStateInformation` per slot to the original bytes.
  Anything less than byte-equality is not a dial.
- **A slot over the 256 KB cap** degrades to null and the UI says which slot.
- **Format mismatch withholds the chunk.** Capture as VST3, force the AU build, assert
  no `setStateInformation` call and a per-slot report.
- **Missing plugin** leaves a named empty slot, never a silent gap.
- **`hasState` false** loads the rack and says settings were not included.
- **Keyboard**: on each host, a focused field swallows space; Escape releases; the
  transport works immediately after.
- **Logic Link-window switch**: no crash, no lost unread count, and under 1b the route
  and scroll are restored.
- **Deadman**: a plugin that dies during state restore is blacklisted with the `state
  restore` phase and withheld on relaunch. That path already exists — assert it fires
  from this new load route too.

---

# 11. Staging, and what is blocked

Nothing here needs to land at once, and the risky part is last on purpose.

| Stage | Contents | Blocked on |
|---|---|---|
| **0** | The 1d survey. Answer all six, report. | Plugin repo free |
| **1** | Backend: 9a, 9b, 9d. | Nothing — can start today |
| **2** | The tab, read-only, plus the badge (absorbs M1.2). | 1a/1b decision, stage 0 |
| **3** | Chain load with full parameter dial — section 2 and 5a. | Stage 2 |
| **4** | Chat and project deep links — 5c. | Stage 2 |
| **5** | Community read: announcements, feed, followers. | Stage 2 |
| **6** | Messaging: send, react, block. Section 4. | Stage 5, keyboard survey |

**Stage 3 before stage 5.** Loading a chain with its settings is the thing the product
owner called essential and vital; community reading is the thing that is merely at
parity with a browser he can already open.

## Blocked on, explicitly

1. **The plugin repo being free.** One worktree per session; four incidents so far,
   one of which cost an entire stretch.
2. **The 1a/1b decision**, which is yours.
3. **Ownership.** Everything to date has been the backend repo. Plugin C++ is a
   different repo and a different discipline, and I should not start in it without you
   saying so.

## Open questions needing your answer

1. **1a or 1b.** Recommendation: 1b, per 1c.
2. **Version-mismatch policy** for state chunks: attempt with the deadman covering it,
   or withhold? Recommendation: attempt, because the deadman now blacklists a plugin
   that dies in `state restore`, so the bad case is bounded.
3. **`listLimit` for `surface=plugin`**: raise to 8, or drop the distinction?
4. **Does the Chain tab confirm before replacing an unsaved rack?** If not, that is a
   prerequisite bug, not part of this.
