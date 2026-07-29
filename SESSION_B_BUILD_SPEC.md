# EchoJay v2 — Session B: chain saving in the plugin

> **OWNER: this repo (echojay-vst-v200).** Edit here. Any copy in echojay-saas
> is a mirror and must not be edited there.

Status: NOT STARTED. Blocked on the plugin repo being free.
Repo: `~/Documents/ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200`
Backend counterpart: `echojay-saas` (the V2 backend, never `echojay-saas-v2`).
Companions: `DASHBOARD_BUILD_SPEC.md`, `CHAIN_AI_BUILD_SPEC.md` (read its Gotchas),
`D1_BUILD_SPEC.md`.

---

# 0. Why this exists, and why it moved up the order

Nothing saves a chain today. D1.1 built `POST /api/v2/chains` so the producer would
exist, but nothing calls it. The web cannot be that producer: the
`<<<ECHOJAY_CHAIN>>>` machine block carries `name`, `role`, `settings` and
`settings_structured` only. Manufacturer, format, version and uid come from the
plugin's local catalog at build time and the backend has never seen them.

So **Session B is a prerequisite for D3, not an optional plugin nicety.** D3 without
a plugin save path has nothing to share. The revised order is:

```
D2 (in progress) -> Session B -> M1 -> D3 -> M2
```

Session B also closes two D1 debt items: chat `updatedAt` stamps, and
`onboarding.builtChain`, which is hardcoded false until `dash.chains` has a row.

## Prerequisites, confirm before starting

1. The composer redesign has landed. Session B touches the Chain tab and the tab
   strip must not be moving underneath it.
2. The param-mapping session is out of `echojay-vst-v200`. **One worktree per
   session**, learned the hard way in D0.
3. `POST /api/v2/chains` is DEPLOYED to production, but it is **not reachable
   there**. Every `/api/v2/*` route is gated on `DASHBOARD_ENABLED`, which is
   still `false`, so the endpoint answers `404 {"error":"not_enabled"}` on
   www.echojay.ai. That is the dark state working as intended, not a fault.

   **The plugin session therefore develops against a PREVIEW deploy with the
   flag on**, not against production. Everything Session B adds to the backend
   sits behind the same flag, so no amount of plugin work makes production
   answer until the flag is flipped.

---

# 1. The two "params" problem

This distinction is the whole reason chain saving can ship now rather than waiting on
Phase 2. Do not conflate them, and keep them in separate fields from the first commit.

| | `state` | `params` |
|---|---|---|
| What | Opaque blob from `getStateInformation` | Structured, readable settings |
| Needs param map | **No** | Yes, blocked at 296/1551 |
| Readable | No | Yes |
| Portable | Same machine, same plugin version | Anywhere |
| Ships | **Now** | Phase 2 |
| In shares | **No, local recall only** | Yes, when it exists |

Setting a *named* parameter ("ratio to 4:1") needs the map, because it has to find
which index is ratio on that plugin. Capturing exactly what a plugin is doing right
now needs nothing: JUCE gives `getStateInformation` and `setStateInformation` on the
hosted instance. Complete, exact, works on every plugin today.

**Personal recall is therefore exact right now.** Save a chain, reopen it next week,
every knob is where you left it.

## Why `state` stays out of shares by default

It embeds version-specific and sometimes machine-specific data, it cannot be
inspected before loading, and handing an opaque blob from a stranger to a plugin's
own parser is not something to do by default. Local first. An explicit "exact
settings, may not load" extra can come later if people ask for it.

---

# 2. No auto-save

Decided and settled: **explicit saves only.**

If the value of a saved chain is recalling it later, a chain you did not choose to
save is not one you want back. Auto-save on confirm would fill the dashboard with
rows nobody asked for and make "your chains" mean "everything you happened to try".

Consequence, accepted: `onboarding.builtChain` stays false until someone explicitly
saves, so the checklist step becomes "Save a chain", which is a real action rather
than a side effect. Update the label in the payload accordingly.

---

# 3. Scope

| Slice | What | Notes |
|---|---|---|
| **B.0** | Chat `updatedAt` stamps on plugin save | Tiny. Closes a D1 debt item. |
| **B.1** | Save and Save As, with state capture | The essential one |
| **B.2** | Load a saved chain, list saved chains | Recall is the point |
| **B.3** | Plugin Dashboard tab | Deferrable, ship B.0 to B.2 first |

B.0 to B.2 are the ones that matter. B.3 was the original Session B and is now the
least important part of it.

---

# 4. B.0 — chat updatedAt stamps

D1 stamps `updatedAt` on chat save from the web only. The plugin sends chats whole
from its own model, so a plugin save **drops the field the web wrote**, and those
chats fall back to creation order in `recentChats`.

Fix: stamp `updatedAt` on the plugin's chat save path too, matching the web's format
exactly (ISO string). Preserve any existing `updatedAt` on chats the plugin did not
touch.

The whole-chat preservation rule from D1.1 applies and is a real hazard: both clients
send `chats` and `albums` whole because each carries fields the other owns. Guarded by
`scripts/test-chat-hierarchy-preservation.mjs` on the backend. Do not drop unknown
fields.

---

# 5. B.1 — saving

## Surface

In the Chain tab. Two actions:

- **Save** updates the currently loaded saved chain, if there is one.
- **Save As** prompts for a name and creates a new one.

If no chain is currently loaded, Save behaves as Save As. Show the loaded chain's
name somewhere persistent so "Save" is never ambiguous about what it overwrites.

## What gets captured

Per slot, the shared chain format from umbrella spec section 5:

```jsonc
{
  "n": 1,                        // 1-BASED, always
  "manufacturer": "Waves",
  "plugin": "Renaissance Vox",
  "format": "VST3",
  "version": "14.0",
  "uid": "1096303697",
  "bypassed": false,
  "role": "level control",
  "params": null                 // reserved for Phase 2, key exists from day one
}
```

Plus, in a **separate field**, never inside `slots`:

```jsonc
"state": { "1": "<base64>", "2": "<base64>", "3": null }
```

Keyed by slot number so a missing or skipped slot is explicit rather than inferred by
position.

## State capture rules

- **Off the audio thread.** `getStateInformation` on a hosted instance can take
  seconds for sampler or convolution plugins. Never call it from `processBlock`.
- Show progress if capture takes longer than a moment. A save that appears to hang is
  worse than a save that says what it is doing.
- **Per-slot cap: 256KB.** Above it, store `null` for that slot and record why.
- **Total cap: 1MB.** If exceeded, keep the smallest states that fit and null the
  rest, largest dropped first.
- **Degrade honestly.** A chain whose states were skipped still saves, and the UI says
  plainly which slots did not capture: "Settings not captured for Kontakt (too
  large)". Never a failed save, never a silent omission.
- Store the plugin `version` alongside each state. Restore needs it.

If state blobs turn out to routinely exceed the caps in real use, the fix is Blob
storage with a URL in the column rather than raising the Postgres limit. Note it,
do not build it.

## Why the SESSION caps are tighter than these, and must stay that way

The caps above (256KB per slot, 1MB total) govern what goes to the API. DAW
session persistence uses **128KB per slot and 512KB total**, decoded, defined as
`kSessionStateMaxSlotBytes` / `kSessionStateMaxTotalBytes` in `Source/ChainHost.h`,
where the same reasoning is written next to the constants.

They differ on purpose. An oversized chain sent to the API is rejected by a server
that answers `413` and the user is told, once. An oversized session has no server
to reject it: it is written into the user's project file on every single save, it
grows the file forever, and there is no undo. The API cap protects a request; the
session cap protects a document. Do not "fix" the inconsistency by aligning them,
in either direction.

## Honesty

Saving is a real action the user took, so "Saved" in past tense is legitimate. But
saving changes nothing about the sound, so the confirmation says what happened
(a chain was saved) and never implies a sonic result. The Action Honesty rule from
`CHAIN_AI_BUILD_SPEC.md` still governs everything the model says around it.

---

# 6. B.2 — loading

- List saved chains in the Chain tab. Name, slot count, when saved.
- Loading builds the rack from `slots`, then applies `state` per slot where present.
- **Version mismatch**: attempt `setStateInformation` anyway, since most plugins
  handle their own versioning. If the plugin rejects it or throws, load the plugin at
  its default and tell the user which slots did not restore. Never fail the whole
  load for one slot.
- **A plugin that will not load means "can't authorise right now", not "not owned".**
  An absent iLok fails plugins the user owns. Session-scoped exclusions only, never
  persisted. This rule is carried from the chain spec and applies here unchanged.
- Run the existing substitution logic for missing plugins: "you have 4 of 6, here are
  alternatives for the other 2".

---

# 7. Backend work

Small, and it belongs in `echojay-saas`, not the plugin repo. Separate session,
separate worktree.

1. **Migration** `migrations/dash/0006_chains_state.sql`: add a `state jsonb` column
   to `dash.chains`. Separate from `slots` so a share serialises `slots` only and
   `state` cannot leak by accident.
2. **`GET /api/v2/chains`**: list for the authenticated account. Name, slot count,
   quality score, project, updated_at. **Does not return `state`**, since a list does
   not need megabytes.
3. **`GET /api/v2/chains/:id`**: full chain including `state`, ownership checked
   against the opaque uid.
4. **`PATCH /api/v2/chains/:id`**: for Save overwriting an existing chain.
5. `POST /api/v2/chains` already exists from D1.1 and gains `state`.
6. **Payload budget**: the dashboard already lists chains and must not gain a query
   or start selecting `state`. Verify the cold build is still 5 Redis reads and 4
   Postgres queries.

## Tier limits, a decision to make

Saving is a Postgres write, not inference, so it does **not** belong in the weighted
unit pool (chat 1, capture 3, chain 4, compare 4, Link 6-10). Same rule as messaging.

But state blobs are real storage. A cap on saved chains for the free tier is
reasonable: something like 10 saved chains free, unlimited on paid. Put it in a config
object read at request time, like `COMMUNITY_LIMITS`, and log every cap hit so it can
be retuned from a deploy. Start tight, loosen later.

---

# 8. Verification

- A chain saves and reloads with every knob where it was left, in a fresh session
  after a DAW restart.
- A plugin whose state exceeds 256KB saves the chain, nulls that slot's state, and
  says so plainly in the UI.
- A chain totalling over 1MB keeps the smallest states and reports which were dropped.
- Loading a chain where one plugin will not authorise loads the rest, offers
  alternatives, and never says "not owned".
- Slot numbers are 1-based everywhere a user or the model sees them.
- `state` never appears in any share payload.
- Chat `updatedAt` survives a plugin save, and a chat the web stamped is not reverted
  to creation order.
- `onboarding.builtChain` becomes true after the first explicit save.
- Dashboard cold build budget unchanged: 5 Redis reads, 4 Postgres queries.
- No non-ASCII literals in draw strings. Explicit escapes only.

---

# 9. Gotchas

Carried from `CHAIN_AI_BUILD_SPEC.md` and still binding:

- **Build via `~/reinstall-v2.sh`.** It kills the AU host, bumps the version,
  rebuilds and installs atomically. The on-screen version is proof of a fresh binary.
- Confirm the binary timestamp is newer than the newest source before installing.
- **Never two Claude Code sessions building the shared repo at once.**
- **Logic recreates the plugin editor** when switching between the Link window and
  EchoJay. State that must survive lives on the processor or in the workspace, never
  on the editor. A save in progress must survive that switch.
- **Height reservation**: `resized()` is the sole geometry author storing a rect,
  `paint()` consumes it and measures nothing. Two diverging `tH` sums caused the
  ASK-chip and ops-card overlaps.
- **Non-ASCII in draw strings** uses explicit escapes (`\xe2\x80\x94`), never literals.
- **Slot numbers are 1-based**; the single conversion is in
  `ChainHost::parseChainEditOps`.
- **A load failure means "can't authorise right now", not "not owned".**
- **Action honesty**: the model changes nothing itself, all prose is a proposal. Past
  tense belongs only to a `result` field after the user confirms, and even there,
  placement only, never sonic or settings claims.

New with Session B:

- **`state` and `params` are different fields and must never be conflated.** `state`
  is opaque and local, `params` is structured and portable. Conflating them is how a
  share promises exact settings and silently delivers nothing.
- **`getStateInformation` never runs on the audio thread.**
- **`state` is never included in a share by default.**
- **Degrade, never fail.** A plugin whose state is too large still saves as part of
  the chain, with an honest note about what was not captured.
- **No auto-save.** Saves are explicit, always.
