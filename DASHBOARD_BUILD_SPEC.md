# EchoJay v2 — Dashboard & Community: build spec

> **MIRROR. OWNER: echojay-saas. DO NOT EDIT HERE.** Edit it in echojay-saas and
> copy the file across. An edit made here is invisible to the owning repo and
> will be silently overwritten by the next copy.

Status: DESIGN AGREED, NOT STARTED
Companion to `CHAIN_AI_BUILD_SPEC.md`. Read that one's Gotchas section too, it still applies.
Keep identical copies in both repos.

## Repos (unchanged, and still inverted)
- Plugin: `~/Documents/ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200`
- Backend: `echojay-saas` — **this is the V2 backend.** `echojay-saas-v2` is LEGACY, never edit it.

---

# 1. What this is

Two connected things, built in one data model:

1. **Dashboard**: the landing surface of the web app, and a new home tab in the plugin.
   Projects, chains, recent chats, usage, continue-where-you-left-off, onboarding.
2. **Community**: an overlay (not a tab) carrying announcements, a team inbox, and
   later, connections and direct messages with chain attachments.

The single defensible thing here is **a chain object that travels between people and
opens in the recipient's plugin.** Discord cannot do that. Everything else on this page
is scaffolding for that one capability.

## Naming decisions (locked)

- **"Chain" stays.** Not "treatment". It is native engineer vocabulary, it is baked into
  prompt v18, `ChainHost::parseChainEditOps`, `rack-<uid>.json`, the docs, the support
  history and the marketing. Renaming buys nothing and reads as downstream of a competitor.
- **A shared chain is a "shared chain"**, addressed by slug. **Do NOT call it a "rack".**
  `rack-<uid>.json` is already the Link sidecar. That collision would poison search across
  both repos.
- **"Chat" is the AI. "Community" is humans.** Never blur these. Different container
  styling, human avatar always present on Community messages. The Action Honesty rule
  belongs to Chat only, and a peer's message must never be able to read as a model proposal.
- **"Connections"**, not friends and not follows. Mutual accept. One graph.

## Non-goals

- No WebView in the plugin. You already had a GPU teardown crash under loader lock on
  Windows/Ableton. A browser component reintroduces exactly that class of risk and adds
  cookie and offline problems.
- No Discord inbound bridge. Push out only. See section 7.
- No browsable member directory. Exact and prefix handle search only.
- No AI image generation at v1. Procedural art, section 4.

---

# 2. Architecture

## One payload, two renderers

`GET /api/v2/dashboard` returns a typed JSON blob. React renders it on web, JUCE renders
it natively. There is never a second source of truth for dashboard content. If the plugin
needs a field the web does not show, it still comes from this payload.

The plugin dashboard is a **thin home, not a mirror**. Anything editorial, social or
text-entry-against-an-index opens the browser.

| | Web | Plugin |
|---|---|---|
| Dashboard: projects, chains, recent chats, usage, continue | yes | yes |
| Onboarding checklist | yes | yes |
| Read announcements and replies | yes | yes |
| Read and send DMs in an existing thread | yes | yes |
| **Attach a chain to a message** | yes | **yes, primary surface** |
| Compose new conversation, people search | yes | no |
| Connection requests, accept, block, report | yes | no |
| Profile and handle editing, avatar upload | yes | no |
| Public chain pages, public profiles | yes | no |

Plugin escalates to web by minting a one-time token through the existing device-code
pairing path and opening `https://echojay.ai/go?t=<token>`, landing logged in. Token is
single use, 120s TTL, bound to the account and to a target path.

## Storage

- **Postgres (Neon)**: new schema `dash`. Separate from the analytics six-table schema.
  Do not join across them in request paths.
- **Redis (Upstash)**: stays auth, tier and usage counters. Adds unread counters and a
  60s cache of the dashboard payload, invalidated on write.
- **Identity lives in Redis, not Postgres.** `dash.profiles.user_id` is the existing
  canonical account key as text. It is a foreign key to nothing. Validate at the API layer.
  Do not migrate auth into Postgres as part of this work.
- **Images**: Vercel Blob. Resize on upload (avatar 512px square, project art 1024px),
  strip EXIF, reject anything over 8MB pre-resize.
- **Plugin image cache**: disk cache under the existing app support dir, hard cap 64MB,
  LRU eviction. Procedural art keeps this near-empty, which is another argument for it.

## Transport for Community

Vercel serverless cannot hold websockets.

- **Web**: Ably (or Pusher). Free tier covers current scale comfortably. Presence and
  unread come for free. Do not hand-roll SSE.
- **Plugin**: **poll, do not socket.** `GET /api/v2/community/poll` every 20s while the
  editor is open. Served from Redis only, never touching Postgres. One Redis GET per user
  per 20s.
- Escalate the plugin to Ably only if latency complaints appear. For a support inbox and
  announcements, they will not.

## Feature flags

Follow the `V2_UI_ENABLED` pattern already in use.

- `DASHBOARD_ENABLED` (web + payload)
- `COMMUNITY_ENABLED` (overlay, both surfaces)
- `COMMUNITY_DM_ENABLED` (M2 only, separate flag so M1 can ship alone)

**None of these is a switch you can throw.** Env is snapshotted into a
deployment at build time, so changing a flag does nothing until a redeploy
lands, on the middleware exactly as much as on the functions. To turn a
surface off in seconds, `vercel rollback` to a deployment built with the flag
off. Determined empirically 29 Jul 2026: `docs/env-flags-and-rollback.md`.

- Plugin reads flags from the dashboard payload, not from a local build constant, so the
  overlay can be dark-launched without a binary release.

---

# 3. Data model

```sql
create extension if not exists citext;
create schema if not exists dash;
```

## Profiles

```sql
create table dash.profiles (
  user_id           text primary key,          -- canonical Redis account key
  handle            citext unique,             -- null until claimed
  display_name      text,
  avatar_url        text,
  bio               text check (char_length(bio) <= 280),
  links             jsonb not null default '[]'::jsonb,   -- [{kind,url}]
  credits           text,                      -- free text, "platinum / gold credited"
  daw               text,
  genres            text[] not null default '{}',
  rig               jsonb,                     -- snapshot from the plugin scanner
  rig_verified_at   timestamptz,
  visibility        text not null default 'public'
                     check (visibility in ('public','unlisted')),
  dm_policy         text not null default 'connections'
                     check (dm_policy in ('connections','anyone','nobody')),
  email_verified_at timestamptz,
  created_at        timestamptz not null default now(),
  updated_at        timestamptz not null default now()
);
create index on dash.profiles (lower(handle) text_pattern_ops);
```

Handle rules: 3 to 24 chars, `[a-z0-9_]`, case-insensitive unique, immutable for 30 days
after being set, reserved list for `admin`, `echojay`, `support`, `team`, `help`, `api`.

## Projects

```sql
create table dash.projects (
  id          uuid primary key default gen_random_uuid(),
  user_id     text not null,
  name        text not null,
  genre       text,
  art_kind    text not null default 'procedural'
                check (art_kind in ('procedural','upload','audio')),
  art_seed    text not null,                   -- hex, immutable, see section 4
  art_url     text,                            -- only when art_kind <> 'procedural'
  archived_at timestamptz,
  created_at  timestamptz not null default now(),
  updated_at  timestamptz not null default now()
);
create index on dash.projects (user_id, archived_at, updated_at desc);
```

Projects are the container the product has been missing. The intake overlay already
collects a project name, so half the model exists. Chats, captures, chains and compares
all get an optional `project_id`.

## Chains

```sql
create table dash.chains (
  id            uuid primary key default gen_random_uuid(),
  user_id       text not null,
  project_id    uuid references dash.projects(id) on delete set null,
  name          text not null,
  slots         jsonb not null,                -- see Shared chain format, section 5
  slots_version int  not null default 1,
  quality_score numeric,
  source        text not null default 'plugin'
                  check (source in ('plugin','web','import')),
  created_at    timestamptz not null default now(),
  updated_at    timestamptz not null default now()
);
create index on dash.chains (user_id, updated_at desc);
```

`dash.chains` is the **durable shareable copy**, written on save or share. It is not the
live plugin rack state and it is not `rack-<uid>.json`. Live state stays where it is.

```sql
create table dash.chain_shares (
  id          uuid primary key default gen_random_uuid(),
  chain_id    uuid not null references dash.chains(id) on delete cascade,
  slug        text unique not null,            -- short, url-safe, 10 chars
  visibility  text not null default 'link'
                check (visibility in ('public','link')),
  title       text,
  notes       text,
  view_count  bigint not null default 0,
  revoked_at  timestamptz,
  created_at  timestamptz not null default now()
);
```

`public` is indexable and eligible for featuring. `link` is unlisted, noindex.

## Connections

```sql
create table dash.connections (
  id           uuid primary key default gen_random_uuid(),
  requester_id text not null,
  addressee_id text not null,
  status       text not null default 'pending'
                 check (status in ('pending','accepted')),
  created_at   timestamptz not null default now(),
  responded_at timestamptz,
  -- ordered pair, so a single row covers both directions
  check (requester_id <> addressee_id)
);
create unique index connections_pair
  on dash.connections (least(requester_id, addressee_id), greatest(requester_id, addressee_id));

create table dash.connection_invites (
  code           text primary key,             -- 12 chars, url-safe
  created_by     text not null,
  uses_remaining int not null default 1,
  expires_at     timestamptz not null,
  created_at     timestamptz not null default now()
);
```

There is no `follows` table and no `discord_links` table. Both were considered and cut.

## Conversations and messages

```sql
create table dash.conversations (
  id             uuid primary key default gen_random_uuid(),
  kind           text not null check (kind in ('channel','team','direct')),
  slug           text unique,                  -- channels only, eg 'announcements'
  title          text,
  description    text,
  posting_policy text not null default 'members'
                   check (posting_policy in ('admin','members')),
  created_at     timestamptz not null default now()
);

create table dash.conversation_members (
  conversation_id uuid not null references dash.conversations(id) on delete cascade,
  user_id         text not null,
  role            text not null default 'member' check (role in ('member','admin')),
  muted           boolean not null default false,
  last_read_at    timestamptz,
  joined_at       timestamptz not null default now(),
  primary key (conversation_id, user_id)
);

create table dash.messages (
  id              uuid primary key default gen_random_uuid(),
  conversation_id uuid not null references dash.conversations(id) on delete cascade,
  author_id       text not null,
  body            text,
  reply_to_id     uuid references dash.messages(id) on delete set null,
  attachment_kind text check (attachment_kind in ('chain','capture','project')),
  attachment      jsonb,                       -- SNAPSHOT, not a reference. See gotchas.
  edited_at       timestamptz,
  deleted_at      timestamptz,                 -- soft delete only
  created_at      timestamptz not null default now()
);
create index on dash.messages (conversation_id, created_at desc);

create table dash.message_reactions (
  message_id uuid not null references dash.messages(id) on delete cascade,
  user_id    text not null,
  emoji      text not null,
  created_at timestamptz not null default now(),
  primary key (message_id, user_id, emoji)
);

create table dash.blocks (
  blocker_id text not null,
  blocked_id text not null,
  created_at timestamptz not null default now(),
  primary key (blocker_id, blocked_id)
);

create table dash.reports (
  id         uuid primary key default gen_random_uuid(),
  message_id uuid not null references dash.messages(id) on delete cascade,
  reporter_id text not null,
  reason     text not null,
  detail     text,
  status     text not null default 'open'
               check (status in ('open','actioned','dismissed')),
  created_at timestamptz not null default now(),
  resolved_at timestamptz
);
```

**Announcements is not a special table.** It is one row:
`kind='channel', slug='announcements', posting_policy='admin'`. Every account is a member.
Replies are ordinary messages with `reply_to_id` set. This is why building it natively is
cheaper than bridging Discord: the plumbing already exists for DMs.

## Notifications

```sql
create table dash.notifications (
  id         uuid primary key default gen_random_uuid(),
  user_id    text not null,
  kind       text not null,      -- 'reply','dm','connection_request','connection_accepted',
                                 -- 'chain_comment','featured','announcement'
  payload    jsonb not null,
  read_at    timestamptz,
  created_at timestamptz not null default now()
);
create index on dash.notifications (user_id, read_at, created_at desc);
```

## Redis keys

```
unread:{userId}                 -> hash { total, announcements, team, direct }
unread:conv:{userId}:{convId}   -> int
dash:payload:{userId}           -> JSON, TTL 60s
rl:msg:min:{userId}             -> int, TTL 60
rl:msg:day:{userId}             -> int, TTL 86400
rl:conn:day:{userId}            -> int, TTL 86400
```

The poll endpoint reads `unread:{userId}` and nothing else.

---

# 4. Procedural project art

Deterministic, no network call, no cost, no latency, no moderation surface, and it renders
equivalently in React and JUCE. **This algorithm is normative. Both renderers must produce
the same image.**

The look is a **soft mesh gradient**: a saturated base with 4 to 6 radial colour blobs in a
tight hue family. Calibrated against reference album art to land at roughly saturation 0.84
to 0.94, lightness 0.55 to 0.64, and a hue spread under 12 degrees. The tight hue family is
what makes a tile read as artwork rather than as generative filler, and it is the single
most important number here. Widen the spread and the tiles immediately look cheap.

## Seed

`art_seed` is set once at project creation and is **immutable**. Renaming a project must not
change its artwork.

```
art_seed = hex( sha256(project.id::text) )[0..15]   // 16 lowercase hex chars
```

## PRNG

A shared mulberry32, seeded from the first 4 bytes. Exact uint32 arithmetic, so the
TypeScript and the C++ consume identical value streams.

```
a = seed32
next():
  a += 0x6d2b79f5                               (uint32 wrap)
  t = a
  t = (t ^ (t >> 15)) * (t | 1)                 (uint32 wrap)
  t ^= t + (t ^ (t >> 7)) * (t | 61)            (uint32 wrap)
  return (t ^ (t >> 14)) / 4294967296
```

JavaScript uses `Math.imul` and `>>>` to force the same 32-bit semantics. **The ORDER of
`next()` calls IS the algorithm.** Reordering or inserting a call on one side silently
changes every tile on that side only. Verified across 505 seeds against a compiled C++
mirror, zero mismatches. Keep `art_parity_fixture.json` as a unit test on the plugin side.

## Derivation

```
hueAnchor = floor(next() * 360)
spread    = 12 + next() * 34          // degrees, tight family
blobCount = 4 + floor(next() * 3)     // 4, 5 or 6

base = hsl(hueAnchor, 0.90, 0.60)

per blob, in this exact order:
  h  = hueAnchor + (next() * 2 - 1) * spread
  s  = 0.84 + next() * 0.15
  l  = 0.54 + next() * 0.14
  cx = 0.08 + next() * 0.84           // fraction of the tile
  cy = 0.08 + next() * 0.84
  r  = 0.42 + next() * 0.46
```

Saturation and lightness bands are **fixed, not seeded**. Every tile is vivid and high-key,
which is what makes it pop against the dark navy UI. This is a deliberate reversal of the
earlier "sit inside the EchoJay palette" rule: album art should contrast with the shell,
not blend into it.

## Rendering

Base fill, then each blob painted over the whole square as a radial gradient from the blob
colour at full opacity to fully transparent, with a mid stop:

```
offset 0     alpha 1.00
offset 0.45  alpha 0.72
offset 1     alpha 0.00
```

The 0.45 mid stop matters. Without it the falloff reads harder and the tile stops looking
like a mesh gradient.

- Web: SVG with stacked `<radialGradient>` fills. Crisp at any size, no canvas taint.
- JUCE: `juce::ColourGradient` in radial mode per blob, filled over the square, drawn into a
  cached `juce::Image` keyed on `art_seed` plus size. LRU, 64 entries.
- Geometry is computed in a fixed 0..1000 space and scaled, so both agree at any size.
- Corner radius only. **No accent overlay and no inner border**, both were tried and both
  cheapen it.
- **Never send the rendered image over the wire.** Send `art_seed` and draw it on each surface.

## Distribution

Verified over 3000 projects: hue anchors uniform to within 11 percent across twelve 30 degree
bins, blob counts evenly split across 4, 5 and 6.

## Later (not v1)

`art_kind = 'audio'`: derive `hueAnchor` from spectral centroid, `spread` from crest factor,
and blob lightness from LUFS, using the capture data already collected. Same blob geometry,
so it is a seed swap and not a new renderer. Project art that is literally the shape of the
mix is a visual identity nobody else can copy.

`art_kind = 'upload'`: user image, Blob storage, `art_url` populated.

AI-generated art, if it ever ships, is a paid action priced at 8 to 10 units, above Link.
Not part of this spec.

# 5. Shared chain format

This is the crown jewel. Get it right the first time, because retrofitting a share format
after Phase 2 param mapping lands is painful.

```jsonc
{
  "v": 1,
  "name": "Drill vocal",
  "slots": [
    {
      "n": 1,                          // 1-BASED. Always. See gotchas.
      "manufacturer": "Waves",
      "plugin": "Renaissance Vox",
      "format": "VST3",
      "version": "14.0",
      "uid": "1096303697",             // host plugin uid where known
      "bypassed": false,
      "role": "level control",
      "params": null                   // reserved for Phase 2, versioned separately
    }
  ],
  "notes": "Placement only. No sonic claims.",
  "sourceTier": "pro"
}
```

## Rules

- **`params` stays `null` until Phase 2 unblocks.** The key must exist from day one so the
  schema does not change when it fills. Param payloads carry their own `paramsVersion`.
- **Slot numbers are 1-based** everywhere a human or the model sees them. The single
  conversion to 0-based stays in `ChainHost::parseChainEditOps`.
- The recipient will not own everything. The share page and the plugin both run the
  existing substitution logic: "you have 4 of 6, here are alternatives for the other 2,
  open in plugin".
- **A missing or failed plugin means "not detected on this system", never "you do not own
  this".** An absent iLok fails plugins the user owns. The share page must never assert
  ownership state. This is the same rule as the load-failure gotcha in the chain spec.
- **A chain page describes placement, never result.** No "this makes vocals sit forward".
  The Action Honesty rule extends to every shared surface.

## Public chain pages

`/c/:slug`. Server-rendered, indexable when `visibility='public'`, `noindex` when `'link'`.
Carries: chain name, slot list with manufacturer and plugin, author profile card, the
substitution view for logged-in users, a **Message the author** button (M2), and an
Open in EchoJay button.

These pages are organic acquisition. "Drill vocal chain with Waves plugins" as an indexable
page is the cheapest signup available, and it is the entry point for the whole social graph.

---

# 6. API surface

All under `/api/v2`. All authenticated through the existing session or device token.

## Dashboard

```
GET  /api/v2/dashboard?surface=web|plugin
```

```jsonc
{
  "v": 1,
  "generatedAt": "2026-07-27T09:00:00Z",
  "ttl": 60,
  "flags": { "community": true, "dm": false },

  "user": {
    "id": "u_123",
    "handle": "seand",
    "displayName": "Sean D",
    "avatarUrl": null,
    "tier": "studio",
    "hasProfile": true
  },

  "usage": {
    "unitsUsed": 412, "unitsTotal": 1200, "pct": 34,
    "resetsAt": "2026-08-01T00:00:00Z",
    "tierLabel": "Studio",
    "upgradeUrl": "/upgrade"
  },

  "continue": {
    "kind": "chat",
    "label": "Drill vocal, bus glue",
    "projectId": "p_1",
    "target": { "surface": "chat", "id": "c_88" },
    "updatedAt": "2026-07-26T22:14:00Z"
  },

  "projects": [
    { "id": "p_1", "name": "Untitled Project", "genre": "drill",
      "art": { "kind": "procedural", "seed": "9f3a1c7d22b40e51", "url": null },
      "counts": { "chats": 4, "captures": 9, "chains": 2 },
      "updatedAt": "2026-07-26T22:14:00Z" }
  ],

  "recentChats": [
    { "id": "c_88", "title": "Bus glue", "snippet": "Try the 1176 after the...",
      "projectId": "p_1", "linkUid": null, "updatedAt": "2026-07-26T22:14:00Z" }
  ],

  "chains": [
    { "id": "ch_5", "name": "Drill vocal", "slotCount": 6, "qualityScore": 82,
      "projectId": "p_1", "shareSlug": null, "updatedAt": "2026-07-25T18:02:00Z" }
  ],

  "community": {
    "unread": { "total": 3, "announcements": 1, "team": 2, "direct": 0 },
    "featured": { "slug": "ab12cd34ef", "name": "Warm 808 bus", "author": "hl8" },
    "discordUrl": "https://discord.gg/..."
  },

  "onboarding": {
    "complete": false,
    "steps": [
      { "key": "scan",    "label": "Scan your plugins",  "done": true,  "target": {"surface":"settings"} },
      { "key": "capture", "label": "Run your first capture", "done": true, "target": {"surface":"meters"} },
      { "key": "chain",   "label": "Build a chain",      "done": false, "target": {"surface":"chain"} },
      { "key": "profile", "label": "Claim your handle",  "done": false, "target": {"surface":"web","path":"/settings/profile"} }
    ]
  },

  "notifications": { "unread": 1 },

  "gates": {
    "canSendConnectionRequest": true,
    "connectionsUsed": 4, "connectionsMax": null,
    "chainSendsUsed": 1, "chainSendsMax": null,
    "invitesRemaining": null
  }
}
```

Notes:

- `target` is an **abstract deep link**, an object each renderer interprets. It is not a
  URL scheme. The plugin switches tab and selects the id. The web app routes. Where the
  plugin cannot handle a target (`"surface":"web"`) it mints a one-time token and opens
  the browser.
- `null` on a `*Max` field means uncapped.
- Cached in Redis for 60s per user, busted on any write to projects, chains, profile,
  usage or unread.

## Projects, chains, profile

```
POST   /api/v2/projects                     { name, genre? }
PATCH  /api/v2/projects/:id                 { name?, genre?, archived? }
POST   /api/v2/projects/:id/art             multipart, sets art_kind='upload'
DELETE /api/v2/projects/:id/art             reverts to procedural, seed unchanged

POST   /api/v2/chains                       { name, projectId?, slots }
PATCH  /api/v2/chains/:id
POST   /api/v2/chains/:id/share             { visibility, title?, notes? } -> { slug }
DELETE /api/v2/chains/:id/share             sets revoked_at
GET    /api/v2/chains/shared/:slug          public, no auth, includes author card

GET    /api/v2/profile/me
PATCH  /api/v2/profile/me                   { handle?, displayName?, bio?, links?, ... }
POST   /api/v2/profile/me/avatar            multipart
POST   /api/v2/profile/me/rig               plugin only, snapshot from the scanner
GET    /api/v2/profile/:handle              public
```

## Community

```
GET  /api/v2/community/poll                       Redis only. Plugin polls this at 20s.
GET  /api/v2/community/conversations
GET  /api/v2/community/conversations/:id/messages?before=&limit=50
POST /api/v2/community/conversations/:id/messages { body, replyToId?, attachment? }
POST /api/v2/community/conversations/direct       { handle } -> { conversationId }
POST /api/v2/community/read                       { conversationId, lastReadMessageId }
POST /api/v2/community/messages/:id/reactions     { emoji }
DEL  /api/v2/community/messages/:id/reactions/:emoji
DEL  /api/v2/community/messages/:id               soft delete, author or admin only
POST /api/v2/community/messages/:id/report        { reason, detail? }

GET  /api/v2/community/people?q=                  exact + prefix handle only, max 10
POST /api/v2/community/connections                { handle }
POST /api/v2/community/connections/:id/accept
DEL  /api/v2/community/connections/:id            decline or remove
POST /api/v2/community/invites                    -> { code, url, expiresAt }
POST /api/v2/community/invites/:code/accept
POST /api/v2/community/blocks                     { userId }
DEL  /api/v2/community/blocks/:userId

POST /api/v2/admin/announcements                  { body } admin only, fires Discord webhook
```

`GET /api/v2/community/poll` response, kept deliberately tiny:

```json
{ "total": 3, "announcements": 1, "team": 2, "direct": 0, "rev": 4412 }
```

`rev` is a monotonic counter. If it has not moved, the plugin does nothing at all.

## Attachments

```jsonc
"attachment": { "kind": "chain", "id": "ch_5" }
```

Server validates ownership, then **materialises a snapshot** of the chain into
`messages.attachment` as the full shared-chain JSON. It does not store a reference.
Editing a chain later must not silently rewrite what you sent someone last month.

---

# 7. Announcements and Discord

**Native canonical, outbound webhook only.**

Building announcements natively costs one conversation row plus an admin post box, because
the messaging plumbing already exists for DMs. Bridging Discord instead would cost: OAuth
per user, a bot with privileged message intent, gateway or polling, Discord markdown
translation, embed and attachment handling, edit and delete sync, reaction sync, an ID
mapping table, rate limit handling, and third-party content rendering that you then have
to moderate. And the fatal one: it would gate your changelog behind having a Discord
account, when only a few hundred of ~10k signups would ever link one.

So:

- `POST /api/v2/admin/announcements` writes the message natively **and** fires an outbound
  Discord webhook so the server sees it too. Roughly ten lines. No OAuth, no bot, no sync.
- Discord stays the open hangout, surfaced as a single link-out card on the dashboard.
- If the server is currently quiet, close it and go native. Do not seed two communities
  with one founder's hours.

Announcements replaces the changelog card entirely. Do not build both.

---

# 8. Tiering and anti-abuse

**Principle: gate scale and outbound reach, never receiving or replying.**

A free user who receives a chain and cannot open or answer it is a broken acquisition
surface. A free user who can fire 200 connection requests is the spam problem.

## Never gated, any tier

- Receiving DMs, reading, replying in an existing thread
- Opening a chain someone sent, including the substitution view
- Accepting connection requests
- Viewing public profiles and public chain pages

## Anti-abuse, everyone (not a paywall)

- Email verified **and** account age >= 24h before any outbound connection request
- 20 messages per minute, 200 per day
- 20 connection requests per day

## Tier config

Single object, read at request time, **not compiled in**. Every number here will be wrong
on the first guess and must be retunable from a deploy rather than a rebuild.

```ts
export const COMMUNITY_LIMITS = {
  free:      { connectionsMax: 15,   pendingOutMax: 5,  chainSendsPerMonth: 3,  invitesPerMonth: 3 },
  pro:       { connectionsMax: null, pendingOutMax: 25, chainSendsPerMonth: null, invitesPerMonth: null },
  studio:    { connectionsMax: null, pendingOutMax: 50, chainSendsPerMonth: null, invitesPerMonth: null },
  studioMax: { connectionsMax: null, pendingOutMax: 50, chainSendsPerMonth: null, invitesPerMonth: null },
} as const;

export const RATE_LIMITS = {
  messagesPerMinute: 20,
  messagesPerDay: 200,
  connectionRequestsPerDay: 20,
} as const;
```

Log **every cap hit** as an analytics event. That is the only way to find out whether 15
connections is strangling the graph or whether nobody is close to it.

**Start tight, loosen later.** Loosening is a nice email. Tightening after people have 400
connections is a support fire.

The chain-send cap is the money moment: it fires exactly when a free user is doing
something valuable and social. "You have sent 3 chains this month" converts far better
than a percentage bar in Settings.

## Messaging is NOT in the weighted unit pool

Chat is 1 unit, capture 3, chain 4, compare 4, Link 6 to 10, because those cost inference.
A DM costs a Redis write. Putting messaging in the pool would make the pool mean two
unrelated things, and would make a producer talking to a mate burn credits they would
rather spend on analysis. Separate counters, separate caps, separate Settings display.

Single exception: **AI summarisation of a thread**, if it ever ships, is inference and goes
back in the pool.

---

# 9. Web surfaces

## Routes

```
/                       -> /dashboard when authed, marketing when not
/dashboard              the new landing surface
/app                    the existing v2 app (chat, meters, chain, compare)
/@:handle               public profile
/c/:slug                public shared chain
/go?t=<token>           one-time token landing from the plugin
/settings/profile       handle, avatar, links, dm policy
```

The **Dashboard ↔ App toggle** is a persistent segmented control in the header, present on
both surfaces. Last surface is remembered in `localStorage`, but `/` always lands on
Dashboard for a fresh session. A returning user who lives in the app should not have to
click twice, and a new user should never miss the dashboard exists.

## Dashboard layout, top to bottom

1. Header: logo, Dashboard/App toggle, community icon with unread dot, avatar menu.
2. Continue card, full width, only when `continue` is non-null.
3. Usage strip: one bar, tier label, upgrade link. Compact, not a hero.
4. Onboarding checklist, only while `onboarding.complete === false`, dismissible.
5. Projects grid, art tiles, 3 up on desktop.
6. Recent chats list, 5 items, "see all".
7. Chains list, 5 items, share state visible.
8. Community row: latest announcement snippet, featured chain, Discord link-out.

## Community overlay

Full-bleed over the app, opened from the header icon, closed with Escape or the X.
Left rail: CHANNELS (`# Announcements`), DIRECT MESSAGES (+ compose), Profile & settings.
Right pane: message list with day dividers, author, role badge, reactions, reply composer.
When `posting_policy='admin'` and the user is not admin, show the locked composer state
with replies still available.

---

# 10. Plugin surfaces

## Dashboard tab

A **new tab**, added to the existing strip (Metering, Visualiser, Chat, Compare, Chain,
Settings). It becomes the default tab on first launch after update, and remembers the last
tab thereafter.

Content, in order: continue button, usage bar, projects strip with art, recent chats list,
featured chain card, community unread badge. Everything is read plus navigate. No text
entry on this tab at all.

## Community overlay

**Overlay, not a tab.** The tab strip is already six deep, and the composer redesign is
mid-flight. A seventh tab means re-laying-out the tab strip at exactly the wrong moment.

Opened from a header icon carrying the unread badge, drawn over the whole editor.

## Geometry: single height source

The chain spec's height-reservation gotcha applies directly and has already cost real time
twice (ASK-chip overlap, ops-card overlap).

- `resized()` is the **sole geometry author**. It computes every rect once and stores them
  in a layout struct on the component.
- `paint()` **consumes the stored rects and measures nothing.** No second `tH` sum. Ever.
- Message row heights are computed once in `resized()` into a `juce::Array<int>` alongside
  a running offset array, and the paint pass indexes that array.

```cpp
struct CommunityLayout {
    juce::Rectangle<int> railBounds, headerBounds, listBounds, composerBounds;
    juce::Array<int> rowHeights;    // per message, computed in resized() only
    juce::Array<int> rowOffsets;    // prefix sums, computed in resized() only
    int totalContentHeight = 0;
};
```

## Threading and lifetime

- The 20s poll timer and the unread count **live on the processor, not the editor.**
  Logic recreates the plugin editor whenever you switch between the Link window and
  EchoJay. Anything on the editor is lost on that switch.
- Poll runs off the message thread, results marshalled back via `MessageManager::callAsync`
  with a `SafePointer` to the editor.
- The editor registers as a listener on the processor for unread changes and does no
  network work of its own.

## Draw strings

Non-ASCII in draw strings must use explicit escapes (`\xe2\x80\x94`), never literals.
Raw bytes already caused mojibake in the build card.

---

# 11. Moderation and compliance

**M1 carries almost none of this**, which is the main reason M1 and M2 are separate
milestones. In M1 only admins post and DMs go only to the team.

**M2 is where the real work sits:**

- Report button on every message, feeding an admin queue (`dash.reports`).
- Block, hiding both directions and killing delivery.
- Rate limits as above, Redis token bucket.
- Soft delete only, so reported content stays reviewable.
- Retention policy and message export on account deletion.
- Default DM policy is **connections**, not anyone.
- Age gate, because a stranger-to-stranger messaging surface changes the minor risk profile.

Because the business is UK-based, M2 turns this into a user-to-user service and the Online
Safety Act duties (illegal content risk assessment, children's access assessment, reporting
and complaints mechanisms) become live obligations. **This is worth a short conversation
with a solicitor before M2 ships, not after.** Nothing in this document is legal advice.
M1 carries none of it.

---

# 12. Phasing

Each phase is independently shippable. D1 is the ship-alone milestone: valuable with zero
social features, and it validates the payload architecture before anything depends on it.

| # | Phase | Contents | Done when |
|---|---|---|---|
| 1 | **D0** | Schema, `/api/v2/dashboard`, Redis cache, web shell, Dashboard ↔ App toggle | Payload returns real data, toggle works, nothing else user-visible |
| 2 | **D1** | Projects, procedural art, recent chats, chains list, usage, continue, onboarding. Web + plugin Dashboard tab | Both renderers draw the same payload, art identical across surfaces |
| 3 | **D2** | Profiles, handles, public profile pages, verified rig | `/@handle` live, handle claimable, rig snapshot from scanner |
| 4 | **M1** | Community overlay, native `#Announcements` (admin post, replies, reactions), Team inbox, unread badge web + plugin, outbound Discord webhook, Resend email on reply | You can post an announcement and 10k users see it in-app |
| 5 | **D3** | Chain sharing: link and public shares, `/c/:slug`, substitution view, Open in EchoJay | A chain sent by URL opens in a recipient's plugin |
| 6 | **M2** | Connections (requests, invites, handle search), DMs, chain attachments, blocks, reports, rate limits, tier caps, age gate | Two users can connect and send each other a working chain |
| 7 | **D4** | Notifications inbox, featured chain of the week | Featured rotates weekly, notifications badge accurate |

## Ordering constraints

- **D2 before M2 is hard.** No handles means no people search means the New Message modal
  is empty on day one. Playhead shipped exactly that and the feature reads as dead.
- **D3 before M2 is strongly preferred.** "Message the author" from a public chain page is
  the single best discovery route into the graph, and it is the reason to have a graph.
- **The shared chain format (section 5) must be settled before Phase 2 param mapping
  lands.** Params want a versioned slot in that format and retrofitting a share format is
  painful.
- **Do the composer redesign first**, regardless. The plugin Dashboard tab and the
  Community composer both inherit whatever the chat container ends up being.
- D0 and D1 collide with nothing currently in flight and can start whenever.

## Discovery routes into the graph (M2), in order of value

1. Message the author from a public chain page
2. Invite link (works before any network exists)
3. `@handle` exact and prefix search, never a browsable directory
4. Featured chain of the week

---

# 13. Gotchas

Carried forward from `CHAIN_AI_BUILD_SPEC.md` and still binding:

- **Never edit `echojay-saas-v2`.** It is the legacy app. `echojay-saas` is V2.
- **Never run two Claude Code sessions building the shared repo at once.** One torn build
  already.
- **Height reservation**: measure and paint must consume ONE height source. `resized()`
  authors geometry, `paint()` consumes stored rects and measures nothing.
- **Non-ASCII in draw strings** uses explicit escapes, never literals.
- **Slot numbers are 1-based** everywhere a human or the model sees them. Single conversion
  in `ChainHost::parseChainEditOps`.
- **A load failure means "can't authorise right now", not "not owned".** Never persist
  exclusions from one. Applies to the share page and the substitution view too.
- **Action honesty**: the model changes nothing itself, all prose is a proposal. Extends to
  every shared surface. A chain page describes placement, never result.
- **Logic recreates the editor** on Link window switches. Poll timer and unread state live
  on the processor.

New to this spec:

- **Do NOT call a shared chain a "rack".** `rack-<uid>.json` is the Link sidecar. The
  collision would poison search across both repos.
- **Attachments are snapshots, not references.** Materialise the full chain JSON into
  `messages.attachment` at send time. Editing a chain later must not rewrite history.
- **Announcements is a `conversations` row**, not a special table and not a Discord mirror.
- **`art_seed` is immutable.** Renaming a project must not change its artwork. Seed from
  `project.id`, never from the name.
- **Never send rendered art over the wire.** Send the seed, render on each surface.
- **Messaging never enters the weighted unit pool.** Separate counters entirely.
- **The poll endpoint touches Redis only.** If it ever reads Postgres, it will melt at 10k
  users on a 20s interval.
- **Chat is the AI, Community is humans**, and the two must be visually unmistakable.
  A peer's opinion must never read as a model proposal.
- **Tier caps live in config, read at request time.** Never compiled into the plugin.
- **Identity stays in Redis.** `dash.profiles.user_id` is a foreign key to nothing.
  Validate at the API layer. Do not migrate auth as part of this work.

---

# 14. Open questions

1. Free-tier connection cap of 15: guess, not data. Instrument it and retune within the
   first month.
2. Whether the Discord server stays open at all once M1 ships. Depends on current activity.
3. Whether the Dashboard tab or Chat is the plugin default tab after the first launch
   post-update. Currently specced as Dashboard once, then last-used.
4. Featured chain curation is manual (editorial control, no UGC moderation load). Revisit
   only if volume makes it a burden.
5. Whether captures get their own dashboard row, or stay nested inside Projects. Currently
   nested.
