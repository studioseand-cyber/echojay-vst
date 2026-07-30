# EchoJay v2 — Session C: the plugin Dashboard tab

> **OWNER: this repo (echojay-vst-v200).** Edit here. Any copy in echojay-saas
> is a mirror and must not be edited there.

Status: **BUILT 29 Jul 2026**, v2.23.60, installed and pending live Logic
verification (section 9). See section 12 for what shipped and the decisions taken.
Repo: `~/Documents/ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200`
**Replaces M1.2** (the plugin unread badge), which is absorbed into this.
Companions: `DASHBOARD_BUILD_SPEC.md` (umbrella), `D1_BUILD_SPEC.md`,
`M1_BUILD_SPEC.md`, `SESSION_B_BUILD_SPEC.md`. Read the Gotchas in
`CHAIN_AI_BUILD_SPEC.md`.

---

# 0. Why this is worth doing now

**Two pieces of finished, verified code have never run.**

`GET /api/v2/dashboard?surface=plugin` was built in D0 and returns deliberately
shorter lists for exactly this consumer. Nothing has ever called it.

`ProjectArt.h` and `ProjectArt.cpp` were ported byte-for-byte from the TypeScript and
have never drawn a tile.

## The two errors in that sentence, which share one root

Corrected 29 Jul 2026. This section used to say the port was "validated against a
505-seed parity fixture with zero mismatches". Two separate things were wrong, and
the pair is more instructive than either alone because **they have the same cause**.

**The 505 was not re-runnable and the fixture was not the thing checked.** The
505-seed run happened once, in a throwaway harness in a container that no longer
exists. The fixture that actually shipped, `art_parity_fixture.json`, holds TEN
entries at 4 decimal places, which is too coarse to detect the float32-versus-double
difference it exists to catch.

**The JUCE file had never been compiled.** `ProjectArt.h` declared
`JUCE_DECLARE_NON_CONSTRUCTABLE_WITH_STATIC_MEMBERS`, a macro that does not exist in
JUCE 8.0.12 or in any version. It failed on the very first build after being added
to a CMake target, on 29 Jul 2026, which is the first time it was ever compiled at
all.

**The shared root: the harness verified a REIMPLEMENTATION, not the artifact.** What
was compiled and checked against the 505 seeds was a standalone C++ mirror of the
PRNG, `prng_parity_reference.cpp`, written for the harness. `ProjectArt.cpp` itself
never went near a compiler. So the run proved that a hand-written copy of the
algorithm agreed with the JavaScript, and said nothing whatsoever about the file that
would ship.

That is exactly the gotcha in `CHAIN_AI_BUILD_SPEC.md`: **whatever a harness supplies
for convenience is exactly what that harness cannot check.** It was written up from
the M1 auth incident, where a Node suite and curl both set the Authorization header
themselves and so could not verify auth. Same shape here, in a different medium: the
harness supplied its own implementation, so it could not verify the implementation.
The M1 note names "a state blob assembled in a test rather than captured from a real
hosted plugin" as the plugin equivalent. A PRNG mirror compiled instead of the real
translation unit belongs on that list too.

Worth recording that the algorithm turned out to be RIGHT: 512 cases, 18372 value
comparisons, zero mismatches, once the file was actually built. The claim was false;
the code was not. Which is the point. A verification that cannot fail for the right
reason also cannot pass for the right reason.

Replaced by `scripts/gen-art-parity-fixture.mjs` in echojay-saas (512 reproducible
cases at full precision, generated from `public/js/art-core.js`) and by
`tools/art_parity_test` here, which calls the shipping `ProjectArt::derive` and runs
on every build-and-install via `~/reinstall-v2.sh`.

The umbrella spec's core claim, **one payload, two renderers**, has only ever had one
renderer. This is the second.

It also **absorbs M1.2 entirely**. Announcements become a card on the dashboard with
an unread dot on the tab label, rather than a separate overlay with its own chrome.
One surface instead of two, and the polling work is the same either way.

The product reason: most of your users live in the plugin inside a DAW and may not
open the web app for weeks. Without this, you post a release note and nobody sees it.

---

# 1. Prerequisites, confirm before starting

1. **The composer redesign has landed.** It was mid-flight when the dashboard work
   began and it moves the tab strip.
2. **The plugin repo is free of the Link session.** One worktree per session. Four
   incidents so far, one of which cost an entire stretch.
3. **`DASHBOARD_ENABLED` is on the preview** the plugin develops against, per
   `session-b-plugin-kickoff.md`. Everything under `/api/v2/*` 404s without it.
4. Note the preview URL changes on every deploy and the plugin caches
   `~/.echojay/dev.json` once per process, so Logic needs a restart after a backend
   deploy. That has already cost debugging time once.

---

# 2. The tab count problem. SURVEYED 29 Jul 2026, and it is not a problem.

You are at seven tabs: Visualisation, Meters, Chat, Compare, Link, Chain, Settings.
Dashboard makes eight, in a window that is often narrow inside a DAW.

This section used to ask for a survey and to warn that eight tabs in compact mode
needed a designed answer, icon-only or scroll or overflow. **Both halves of that were
wrong.** Measured, recorded here so nobody re-opens it:

**COMPACT MODE DRAWS NO TAB STRIP AT ALL.** `PluginEditor.cpp` gates the whole strip
on `!visualOnlyMode && !compactMode`: "Neither mini mode shows navigation, chat-only
and visual-only are single-purpose windows; expand to get the tabs back." Compact mode
is a chat-only window, `setResizeLimits(420, 500, 600, 900)`, opening at 450x550.
There are no tabs to fit, so there is nothing to design.

**The real constraint is the 900px full-mode floor**, `setResizeLimits(900, 580, 1800,
1200)`, and 900x580 is directly reachable from the window-size menu.

| width | 7 tabs | 8 tabs |
|---|---|---|
| 900, the floor | 128px | **112px** |
| 1170, default | 167px | 146px |
| 1400, large preset | 200px | 175px |

Labels are `EchoJayChrome::kLabelPt = 10.0f` bold. The longest, `VISUALISATION` at 13
characters, needs about 81px. **Eight tabs fit at the floor with roughly 30px of
slack.** `DASHBOARD` is 9 characters and is nowhere near the worst case. No icon-only
mode, no scrolling, no overflow menu. Do not add one.

## What DID need fixing, and is prerequisite work

The strip had **two independent geometry authorities**, each with its own hardcoded
count, eleven thousand lines apart:

```
paint()      constexpr int kTabCount = 7;  int tabW = W / kTabCount;
mouseDown()  constexpr int kTabCount = 7;  int tabW = getWidth() / kTabCount;
```

plus a third silent dependency, `switchToTab(static_cast<Tab>(idx))`, which assumes
the label array order matches the `Tab` enum order. Change one of the three and every
click lands on the wrong tab, with nothing to see. That is the same shape as the four
overlap bugs, and adding a tab is exactly the edit that sets it off.

It was also a live violation of section 8: `paint()` was measuring.

Fixed as its OWN COMMIT before the Dashboard tab was added, so it is separable and
revertible on its own: `resized()` is the sole author and stores the rects,
`paint()` and `mouseDown()` both consume them and measure nothing, and the count is
derived from the label array with a `static_assert` tying it to the enum.

## Placement

- Dashboard is the **first** tab.
- It is the default on first launch after update, then last-tab-remembered thereafter.
  That is what the umbrella spec always said and it matches the web's behaviour.

---

# 3. Thin, not a mirror

The umbrella spec's rule holds and it is what keeps this a manageable slice:

**Read and navigate only. No text entry anywhere on this tab.**

| Shows | Source |
|---|---|
| Usage bar, tier label | `payload.usage`, `pct` is pre-rounded server side, do not recompute |
| Continue card | `payload.continue` |
| Projects, with procedural art | `payload.projects`, art from `ProjectArt::getCached` |
| Recent chats | `payload.recentChats` |
| Chains | `payload.chains` |
| Onboarding checklist | `payload.onboarding`, only while incomplete |
| Latest announcement | `payload.community.latestAnnouncement` |
| Unread dot on the tab label | the poll, see section 5 |

Anything editorial, social or requiring typing **opens the browser**: profile editing,
the full community surface, public chain pages, settings.

---

# 4. Deep links, and what the plugin can actually do

The payload's `DeepLink` is an abstract target, not a URL scheme. Each renderer
interprets it.

- `surface: 'chat' | 'meters' | 'chain' | 'compare' | ...` means switch tab and select
  the id.
- `surface: 'web'` means open the browser.

## SURVEYED 29 Jul 2026: the token path does not exist, in either repo

Searched both sides. There is no `api/go.js`, no `/go` rewrite in `vercel.json`, and
no token-minting endpoint in echojay-saas. Every browser launch in the plugin today
is a bare URL with no auth: `/support`, `/manual`, `/app`, `/upgrade`, `/?noredirect`.

The device-code flow in `EchoJayAPI.cpp` runs the OPPOSITE direction: the plugin mints
a device code, the user verifies in a browser, and the plugin polls `/api/device/poll`
to receive a token. It gets auth INTO the plugin and cannot carry auth out.

**So Session C links out only to things that work WITHOUT auth**, and offers nothing
that would land on a login screen. What that leaves:

| wanted | needs | verdict |
|---|---|---|
| Public chain page `/c/:slug` | nothing | **works**, public since D3.1 |
| Public profile `/@handle` | a claimed handle, profile public | **works** when both hold |
| "Claim your handle" | `/settings/profile` | inert text, not a button |
| Announcement "read more" | community surface | not offered, no public URL |
| Chat deep links | `?view=chat&id=` plus a session | not offered |
| Project tiles | no web project view exists | nothing to link to |

`/upgrade` already opens from the plugin signed-out and is pre-existing behaviour, not
something this tab adds.

The onboarding checklist therefore renders "Claim your handle" as a line of text
saying what is left to do, never as a button. A button that promises a destination it
cannot reach is worse than text.

Building the token path is a BACKEND SLICE in echojay-saas, queued and deliberately
not attempted here. It is worth roughly five surfaces today and more once M2 lands.

---

# 5. Polling, and why there are two intervals

Two different costs, so two different cadences:

| | Endpoint | Interval | Cost |
|---|---|---|---|
| Unread dot | `/api/v2/community/poll` | 20s while the editor is open | 1 Redis round trip, 0 Postgres |
| Full payload | `/api/v2/dashboard?surface=plugin` | on tab open, and its 60s TTL | 5 Redis, 4 Postgres on a cold build |

**Do not refetch the whole payload every 20 seconds.** The dashboard payload contains
`community.unread`, which makes it tempting, but it is a hundred times the cost of the
poll and the poll exists precisely so the badge is cheap at 11,285 accounts.

The poll's `rev` field is a monotonic counter. If it has not moved, do nothing at all.

## The Logic constraint, which is not optional

**The poll timer and the unread count live on the processor, never the editor.** Logic
recreates the plugin editor whenever you switch between the Link window and EchoJay.
Anything on the editor is destroyed on that switch, and this is the standing gotcha
that rule exists for.

- Poll off the message thread, marshal results back with `MessageManager::callAsync`
  and a `SafePointer`.
- The editor registers as a listener on the processor for unread changes and does no
  network work of its own.
- Session B's chain sidebar already had to solve the same shape. Follow it.

---

# 6. Offline

Session B built a disk cache for the chains list, with a "Last updated 14:32" label
and a clear distinction between "no connection" and "this no longer exists". **Reuse
that pattern rather than inventing a second one.**

- Cache the last successful payload per account.
- Render it immediately on opening the tab, refresh behind it.
- A failed refresh keeps the cached view labelled with its time, never replaced by an
  error.
- Actions that genuinely need the network fail honestly and say why.

---

# 7. Procedural art

`ProjectArt` is written and parity-verified. This is its first use.

- `ProjectArt::getCached(seed, edgePx)`, message thread only, 64-entry LRU.
- The payload sends `art.seed` and `art.kind`. **Never fetch a rendered image.**
- `art.kind == 'upload'` means draw `art.url` instead, cover-cropped square. D1.2
  found the web renderer needed `object-fit: cover` for exactly this, and the same
  case applies here.
- Non-square uploads must cover-crop, never letterbox or squash. People upload album
  covers.
- If the parity fixture is not already a unit test in this repo, make it one. It is
  the only thing that catches the two renderers drifting apart.

---

# 8. Geometry, which has now produced four overlap bugs

This file specifically. The rules are not general advice:

- `resized()` is the **sole geometry author**, storing rects. `paint()` consumes the
  stored rects and **measures nothing**. Two diverging `tH` sums caused the ASK-chip
  and ops-card overlaps.
- **Visibility is authored unconditionally**, with the tab test inside the visibility
  expression. A component whose bounds are set only inside `if (currentTab == X)`
  keeps `visible=true` at the wrong coordinates on every other tab. That was the
  Chain header Save buttons drawing over Compare.
- **Every `setVisible` is written per component, never through a pointer loop.**
  `for (auto* b : {...}) b->setVisible(x)` is invisible to a grep for
  `<name>.setVisible`, which is how that bug survived its own author's audit.
- **One right-anchor per strip.** Two controls right-anchored to the same edge from
  separate blocks is what put `Aa` on top of the `CHAINS` switch. One authority places
  both.
- Non-ASCII in draw strings uses explicit escapes (`\xe2\x80\x94`), never literals.

---

# 9. Verification

- The tab renders from the real payload against a preview, for a populated account and
  a fresh one.
- **Art parity**: the same `art_seed` renders identically in the plugin and on the web.
  That is the whole point of the shared algorithm and it has never been checked
  visually across both surfaces.
- The unread dot appears when an announcement is posted and clears on read.
- **The poll timer survives a Link window switch in Logic.** This is the
  editor-recreation case and it is the one most likely to be got wrong.
- Deep links switch to the right tab and select the right item.
- Offline: cached view with its timestamp, no error state replacing content.
- Switch to every other tab and confirm nothing from the dashboard leaks. Four bugs
  say do this properly.
- Compact mode at the narrowest width people use.

---

# 10. Later, deliberately not now

Ship the thin version first. These build on it once it exists and real use has shaped
it:

- **Trending chains.** Needs D3, because trending means publicly shared chains and
  none exist until sharing does. D3 has now landed (`/c/:slug` is public and the
  share chip is already rendered on any chain row carrying a `shareSlug`), so this
  is unblocked and waiting on a payload field rather than on a feature. It is also
  the most interesting card on this surface once it can exist, since it is the only
  one showing other people's work.
- **Featured chain of the week.** `payload.community.featured` is already shaped and
  returning null. Curated by you, so no moderation load.
- Reactions on announcements from inside the plugin.
- A fuller community surface, if the read-only announcements card proves people want
  more.

## QUEUED, visual pass: the continue card and the chat rows truncate mid-word

Seen 29 Jul 2026 on the real payload: `Test project, build a chain with bx_digital
V3 mi`, and rows like `bx_digital V3 mix cut 2 dB at 250 H`.

**IT IS NOT A RENDERING BUG AND CANNOT BE FIXED IN THIS TAB.** The string arrives
already cut. Chat titles are minted at `PluginEditor.cpp` `sendChatMessage` with
`msg.trim().substring(0, 35)`, a hard character count with no word boundary, and
the web app applies the same 35-char rule to the same synced `title` field.
`lib/dash/adapters.js` `getRecentChats` and `getContinue` pass that stored title
through untouched, so the payload carries the truncation and every renderer shows
it. A fix in `DashboardTab.cpp` would be dressing a wound in the wrong renderer,
and the chat sidebar and the web dashboard would keep showing the cut version.

Fix at the mint site, both sides in the same change: cut on the last word boundary
at or before 35 and append an ellipsis. Note that this only helps NEW chats.
Existing titles are already stored truncated and would need a backfill or a
rename, which is why this is worth doing sooner rather than later.

The 80-char message SNIPPET in `chatSnippet` is a separate cut and already ends in
`...`; it is the TITLE that has no ellipsis and no word boundary.

## UNBLOCKED 30 Jul 2026, plugin side: `hasState` on dashboard chain rows

The dashboard chain rows cannot say which chains restore your knobs, so the only
way to find out is to load one, which is the destructive operation. Session B
solved exactly this for the Chain sidebar: `GET /api/v2/chains` returns `hasState`
per row precisely so the UI can answer that without fetching a megabyte to find
out, and `applyChainRows` reads it.

The dashboard payload does not carry it. `DashboardChain` in `lib/dash/types.js`
has `id, name, slotCount, qualityScore, projectId, source, shareSlug,
shareVisibility, updatedAt` and no `hasState`, and the chains query in
`lib/dash/payload.js` does not select it. So this is a payload gap of the same
shape as `profileVisibility` above, not a plugin omission: the plugin has nothing
to draw.

**THE SERVER SENDS IT NOW.** Landed on echojay-saas `main` in the D3.2 backend
merge: `(c.state is not null) as has_state` in the chains query, `hasState` on
`DashboardChain` in `types.js`. No extra round trip and never the blob itself. Not
deployed at time of writing, so it reaches the plugin on the next production deploy.

What is left is PLUGIN SIDE and small: parse `hasState` in `DashPayload::parse`
(the field sits beside `slotCount`, which is already read) and put it on the chain
row, using the SAME wording the Chain sidebar uses so the two surfaces cannot
describe the same chain differently. Deliberately not done in the session that
added the field, because it is the other repo.

## QUEUED, backend: `community.latestAnnouncement` is null on every branch

`lib/dash/payload.js` sets it to `null` with the comment "Populated in M1", and it
is still null on `main` and on every feature branch including `feat/m1-community`,
even though the M1 endpoints (`api/v2/community/*`, `api/v2/admin/announcements.js`)
exist and the unread counters are real.

Two consequences, both already live in the shipped tab and both deliberate:

- The announcement card is written, tested and **never renders**. It is gated on
  the field being non-null, which is the honest gate: an empty card would teach
  people the surface is broken.
- The unread dot cannot be cleared honestly from inside the plugin. It tracks the
  server's counts and clears when the server says they were read, because there is
  nothing on this surface for a user to have seen. The moment the field is
  populated, revisit the decision in section 12: clearing on tab open becomes
  defensible, and `getDashUnreadGeneration` is already wired for it.

This is the single highest-value item on this list, because it is what the whole
poll, badge and card machinery was built for. Section 0's product reason (you post
a release note and nobody sees it) is still true today.

## UNBLOCKED 30 Jul 2026, plugin side: `@handle` can become a link

Found 29 Jul 2026 while building the tab. A GAP, not a limitation, and small.

`/@handle` is one of only two pages the plugin can link to without auth, and the
plugin currently links to NEITHER a public profile nor an unlisted one, because it
cannot tell them apart. `dash.profiles.visibility` defaults to `unlisted`
(migration 0005) and `api/v2/public-profile.js` answers an unlisted handle with the
same 404 as an unknown one, deliberately, so that an anonymous visitor cannot
enumerate taken handles. `payload.user` carries `handle` and `hasProfile` and no
visibility field, so the only safe reading of a handle from the plugin is "assume it
404s", and Session C therefore renders `@handle` as plain identity text.

**THE SERVER SENDS IT NOW.** Landed on echojay-saas `main` in the D3.2 backend
merge: `visibility` joins `handle, display_name, avatar_url` in the profiles query
and `profileVisibility` is on `DashboardUser`. It leaks nothing an owner does not
already know about their own profile. Not deployed at time of writing.

What is left is PLUGIN SIDE and is a one-line condition: parse `profileVisibility`
in `DashPayload::parse`, then link `@handle` when the handle is set AND visibility
is `public`, keeping the current inert text otherwise. Note the asymmetry with
`authorCard` in `shares.js`, which withholds a stranger's handle entirely: here the
viewer IS the owner, so only linkability is in question, never whether to show it.
Nothing else on this surface changes.

Worth doing with the plugin-to-web token path (section 4), which is the larger
queued backend slice and unlocks roughly five more surfaces. This one is worth
doing on its own too, because it is small and the plugin already has the handle.

---

# 11. Gotchas

Carried forward and binding:

- **Build via `~/reinstall-v2.sh`.** CORRECTED 29 Jul 2026: this used to say "the
  on-screen version is the proof of a fresh binary", which directly contradicts the
  standing rule in `CHAIN_AI_BUILD_SPEC.md` and is WRONG. That rule cost a full
  afternoon: an installed component read v2.23.99 while containing object code older
  than three sessions' work, because the version counter and the file timestamp churn
  independently of the linked object code. **Binary verification is a CONTENT check**,
  `strings` the installed Mach-O for a marker the feature under test actually emits.
  Neither the version number nor the mtime is evidence.
- `~/reinstall-v2.sh` now takes an atomic `mkdir ~/.echojay-build.lock` before it does
  anything, because the install destination is shared across worktrees and whoever
  builds last wins. It also runs `tools/tabstrip_test` and `tools/art_parity_test`
  between a green build and the install, so a geometry or parity regression never
  reaches a DAW.
- **Never two Claude Code sessions building the shared repo at once.**
- **Logic recreates the editor** on Link window switches. Timers and state live on the
  processor.
- **Any test that constructs auth by hand cannot verify auth.** M1 shipped a surface
  where every layer passed while a signed-in user got 401 on every request, because
  the Node suite and curl both set the header themselves.
- A scripted edit that fails its assertion has not applied. Verify before continuing.
- No em-dashes anywhere including comments, checked **before** committing.
- `state` and `params` are different things.

New with Session C:

- **The plugin dashboard is thin, not a mirror.** No text entry. The moment it grows a
  form it stops being a day's work.
- **Never refetch the whole payload for a badge.** The poll is one Redis round trip;
  the payload is five reads and four Postgres queries.
- **Never fetch rendered art.** Send the seed, draw it locally, and keep the parity
  fixture as a test.

---

# 12. What shipped, 29 Jul 2026 (v2.23.60)

## Files

| | |
|---|---|
| `Source/DashboardTab.h/.cpp` | the payload parse and the whole surface, as ONE child component |
| `Source/PluginEditor.*` | tab, fetch, cache, deep links, art fetch, unread dot |
| `Source/PluginProcessor.h` | `lastTabIndex`, so an editor recreate returns to the tab |
| `tools/dashboard_test/` | geometry and hit-routing self-test, wired into `~/reinstall-v2.sh` |

`Tab::Dashboard` is first in the enum and `"DASHBOARD"` first in `kTabNames`; the
existing `static_assert` ties them. `tabstrip_test` now reports 8 tabs, 112px each
at the 900px floor, every column routing, which is the measurement section 2
predicted.

## The rules, and where each one is enforced

- **Geometry.** `DashboardView::layout(width)` is the sole author. It clears every
  rect at the top and re-authors on every pass, so a card that is not shown cannot
  keep last pass's coordinates. `paint()` and `zoneIndexAt()` consume. There is one
  right-anchored group on the usage strip, written in one block.
- **Visibility.** `dashViewport_` gets bounds AND visibility UNCONDITIONALLY in
  `resized()`, with the tab test inside the single visibility expression. Every
  `setVisible` in the `Tab::Dashboard` case is written out per component, never
  through a pointer loop.
- **Leaks.** The surface is one child, so hiding the viewport hides all of it.
  Three second authorities also had to learn about this tab, and each would have
  been a bug: `computeColumns` (no chat column, which also turns off
  `assistantSidebarVisible` and every assistant-drawn overlay),
  `updateOnboardingPrompts` (a 20Hz re-shower of the chat input row), and
  `assistantInputContext` (the upgrade button would have placed itself over
  `chatInput`'s stale bounds for any free user out of messages).
- **Fetch cadence.** `fetchDashboard` runs on tab open and after the payload's own
  TTL, and from nowhere else. There is no dashboard timer. `onDashUnreadChanged`
  repaints and does nothing else, with the reason written at the call site.
- **Cache.** `dashboard_cache.json` beside `chain_list_cache.json`, the same
  account-keyed pattern with epoch millis, rendered before any refresh; a failed
  refresh keeps the content and turns the timestamp amber.
- **Art.** `ProjectArt::getCached` for procedural seeds. Uploads fetch the image
  once per url and cover-crop it; a failure is recorded so it is not retried, and
  the seed art draws underneath, which is why the payload sends a seed for uploads.

## Decisions taken, with reasons

- **The unread dot tracks the server's COUNTS, not the generation.** The generation
  says when to repaint and that is all it can honestly say. Clearing the dot because
  the user opened this tab would hide a notification they never saw:
  `community.latestAnnouncement` is still `null` on every branch of echojay-saas, so
  there is nothing on this surface for them to have seen. Revisit when M1 starts
  populating it.
- **A chain row does not open the chain.** It lands on the Chain tab with the saved
  chains list open. `openSavedChain` clears the rack and rebuilds it, destroying the
  state of every hosted plugin; that is the explicit purpose of a click on the Chain
  tab and is not a thing a home screen should do to a mis-click.
- **`@handle` is text, not a link.** `dash.profiles.visibility` defaults to
  `unlisted` (migration 0005) and `/@handle` returns the same 404 for unlisted as for
  unknown, so linking it would 404 for most users. The payload carries no visibility
  field to test. Only `/c/:slug` and `/upgrade` are offered. That missing field is a
  GAP rather than a limitation and is queued in section 10.
- **Last tab lives on the processor.** Not persisted into the state blob. It answers
  "put me back where I was in this session", and a fresh instance reading 0 is what
  makes Dashboard the default on first launch after update. Post-login now lands on
  Dashboard rather than Visualisation.
- **The surface scrolls.** A populated account lays out 776px tall against the 510px
  of content the 900x580 floor gives. A Viewport was cheaper than fighting for room.

## Verified

- `tabstrip_test`, `art_parity_test`, `dashboard_test` all pass, run between a green
  build and the install by `~/reinstall-v2.sh`.
- **Negative control**: `strings` on the installed Mach-O for
  `/api/v2/dashboard?surface=plugin` was 0 before (the inline `fetchDashboard` had no
  caller, so the literal was never emitted) and is 4 now.
- **The real payload**, fetched from the preview with the plugin's own stored token,
  run through the shipping `DashPayload::parse` and `layout`: 5 projects, 5 recent
  chats, 2 chains, 4 onboarding steps, continue card present, announcement null. No
  zone escaped the content box, every zone routed to itself, no zone carried a `web`
  target. A hand-written fixture cannot check the contract; this can.
- Compiles both with and without `ECHOJAY_DEV_TRANSPORT`. Note the shared build tree
  is configured `ECHOJAY_DEV_TRANSPORT=ON`, so lifting flags from
  `compile_commands.json` gives you the DEV variant twice unless you strip the define.

## NOT verified, and it needs Logic

Everything in section 9 that requires a host: the tab drawn from a real payload for a
populated and a fresh account, art parity checked visually against the web, the dot
appearing and clearing, the poll surviving a Link window switch, deep links landing,
the offline path, and the sweep of every other tab for leaks.
