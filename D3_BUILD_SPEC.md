# EchoJay v2 — D3 Chain Sharing: build spec

> **MIRROR. OWNER: echojay-saas. DO NOT EDIT HERE.** Edit it in echojay-saas and
> copy the file across. An edit made here is invisible to the owning repo and
> will be silently overwritten by the next copy.

Status: NOT STARTED. D0, D1, D2 deployed dark. Session B complete. M1 complete bar
the plugin badge.
Companions: `DASHBOARD_BUILD_SPEC.md` (umbrella, section 5 is the chain format),
`D2_BUILD_SPEC.md`, `SESSION_B_BUILD_SPEC.md`, `M1_BUILD_SPEC.md`.
Read the Gotchas in `CHAIN_AI_BUILD_SPEC.md`.

---

# 0. Why D3 matters more than it looks

Everything before this was infrastructure for one person. D3 is the first thing that
travels between people, and it is the payoff for three specific pieces of earlier work:

- **Session B's `state` capture**, which means a shared chain carries exact settings
  rather than a list of plugin names. That is the difference between a recipe and a
  preset, and it is what makes sharing worth doing at all.
- **D2's handles and public pages**, which give a shared chain an author.
- **D0's `dash.chain_shares` table**, which has been sitting empty and correct since
  the first migration.

It is also your **first acquisition surface**. An indexable page reading "Drill vocal
chain, Waves Renaissance Vox into an 1176 into an L2" is a page somebody searches for.
Every other surface in this build serves people who already signed up.

## The one-sentence product

Send someone a link. They see your chain, with the settings. They click Open in
EchoJay and it appears in their plugin, with substitutes offered for anything they do
not have.

---

# 1. What already exists

| | Where | State |
|---|---|---|
| `dash.chain_shares` | migration 0001 | empty, `on delete cascade` on `chain_id` |
| `dash.chains.slots` | 0001 | shared chain format, `params` reserved null |
| `dash.chains.state` | 0006 | exact settings, separate column, never in a list response |
| `dash.chains.source` | 0001 | already has `'import'` as a permitted value |
| `dash.chains.archived_at` | 0008 | soft delete |
| `/@handle` rewrite | D2.3 | proven empirically, `api/` function returning HTML |
| `lib/dash/html.js` | D2.3 | shared escaper, proven against the XSS suite |
| Substitution logic | plugin | "you have 4 of 6, here are alternatives" |
| Chains sidebar | Session B | Favourites and Saved, with **Imported designed for and not rendered** |

So D3 is: one migration, a share endpoint, a public page, an import path, and the
plugin's Imported section finally rendering.

---

# 2. Decisions

## Live while browsing, snapshot on import

A **share** points at your chain. Edit it and the page updates. That is what a link
means, and it lets you fix a mistake in something you have already sent.

An **import** takes a copy. The recipient gets their own row with `source='import'`,
which they can rename, favourite and delete. It does not change under them when you
edit yours, and it survives you deleting yours.

The line is clean and it matches the umbrella spec's snapshot rule for message
attachments. It also means **revoking a share does not unimport it**, which must be
stated plainly in the UI: you cannot unshare what somebody already has, same as any
file.

## Substitution happens in the plugin, never on the web page

This is the decision that keeps D3 private and small.

Telling a visitor "you have 4 of 6" requires knowing their full plugin inventory.
`dash.profiles.rig` is deliberately manufacturer counts only, because a complete
inventory is a lot of information about somebody's studio (D2, section 5). Storing
one server-side to power a web feature would undo that decision for a convenience.

So the **web page shows the chain and nothing about the viewer.** The plugin, which
already holds the catalog and already has the substitution logic, does the matching
after import. The page says "Open in EchoJay to see what you have", which is honest
and needs no new data.

## Public shares require a claimed handle. Link shares do not.

A public chain page is indexable, so it has your name on it. That should require a
handle you deliberately claimed, not a display name that might be a prefill.

A link share is unlisted, `noindex`, and only reachable by someone you sent it to. No
handle needed.

## The unlisted author problem, which D2 deferred to here

D2.3 chose that unlisted profiles return a byte-identical 404, so an unlisted author
has no page to link to. That was the right call then and it does not need reversing.

On a chain page:

- **Author has a public profile**: display name and `@handle`, linked.
- **Author is unlisted or has no handle**: display name only, as text, no link, no
  handle shown. Do not render a link that 404s, and do not reveal a handle whose page
  does not exist.
- **At publish time**, if the author's profile is unlisted, offer to make it public.
  Offer, not require: a public chain with an unlinked author is a perfectly good state.

---

# 3. Migration 0010

Two things, and the first is a D2 debt that comes due here.

## Retired handles

Changing a handle currently frees the old one, so a link someone saved could later
resolve to a different person. Harmless while nothing links to profiles. D3 makes
chain pages link to authors, so it stops being harmless.

```sql
create table dash.retired_handles (
  handle      citext primary key,
  user_id     text not null,
  retired_at  timestamptz not null default now()
);
```

- On handle change, insert the old handle.
- The claim validator rejects any handle in this table, **except** for the account
  that retired it. Getting your own old handle back is reasonable; taking someone
  else's is the problem.
- Never expires. A handle is cheap to keep reserved and expensive to have point at the
  wrong person.
- The existing reserved-handle trigger stays. This is a second, different list.

## Share slug

`dash.chain_shares.slug` exists. Confirm it is generated from a CSPRNG and is not
sequential or guessable, because a guessable slug makes every link share public.
10 url-safe characters is enough at this scale.

Also add an index on `chain_shares (slug) where revoked_at is null` if `0001` did not
already create one. Check before writing it, same as M1.

---

# 4. Backend

```
POST   /api/v2/chains/:id/share      { visibility, title?, notes? } -> { slug, url }
DELETE /api/v2/chains/:id/share      sets revoked_at
GET    /api/v2/shares/:slug          public, no auth, the chain as JSON
POST   /api/v2/shares/:slug/import   authed, copies into the caller's chains
GET    /c/:slug                      the public HTML page, via a rewrite
```

## Rules

- **A share of an archived chain 404s.** Soft delete means the row survives, so this
  needs an explicit filter, and it is exactly the kind of thing a later query drops.
  Same discipline as the free-tier cap filtering `archived_at`.
- **Public means indexable, link means `noindex`.** Two visibilities, one column,
  already in the schema.
- **`GET /api/v2/shares/:slug` returns `state`.** This is the second endpoint that
  ever does, after `GET /api/v2/chains/:id`. Every other read path must stay
  state-free, and the `select *` prohibition still holds.
- **Import creates a copy**, with `source='import'`, a new id, the sender's `slots`
  and `state`, and the recipient's ownership. It does not reference the original.
- **Imports do not count against the free tier's 10 saved chains.** The umbrella
  spec's principle is gate outbound reach, never receiving. Someone who imports 12
  chains and cannot save their own has been punished for being sent things. Filter
  `source='import'` out of the cap query and say so explicitly in the summary,
  because this is the second cap-filter rule and both get silently reintroduced.
- **Importing your own share is a no-op**, not an error and not a duplicate row.
- View counts go to **Redis**, not a Postgres write on every page load. Public pages
  get bot traffic and a write per view is a write per bot.

## Sharing caps

From the umbrella spec's tier config: free tier gets 3 chain sends per month, paid is
uncapped. Put it in the same config object as `COMMUNITY_LIMITS`, read at request
time, and log every cap hit.

This is the deliberate conversion moment: it fires exactly when a free user is doing
something valuable and social, which converts far better than a usage bar.

---

# 5. The public page

`/c/:slug`, server-rendered by an `api/` function with a `vercel.json` rewrite,
exactly the pattern D2.3 proved for `/@handle`.

## Contents

- Chain name, and the author card per section 2's unlisted rules
- The slot list: position, manufacturer, plugin, format. **Placement only.**
- Whether exact settings are included, as a fact about the share, not a promise about
  the result
- Open in EchoJay
- Room left for M2's Message the author

## Honesty, which is load-bearing here

- **A chain page describes placement, never result.** No "this makes vocals sit
  forward". The Action Honesty rule extends to every shared surface and this is the
  most public one.
- **Never assert ownership state.** The page does not know what the viewer owns and
  must not imply it does. When the plugin later reports a plugin will not load, that
  means **can't authorise right now**, never "not owned", because an absent iLok fails
  plugins you own.
- **Never say "restored" about settings that may not restore.** Session B section 6b
  lists the four ways restore fails, external file references being the most common.
  The page can say settings are included. Only the plugin, after loading, can say what
  actually happened.

## Escaping

Chain names, titles and notes are user input rendered into HTML on a page strangers
visit. Same standard as D2.3: one shared escaper, every interpolation including meta
and Open Graph tags, and the verification is a chain whose every field is an XSS
payload, checked with a real parser, with the raw HTML source shown rather than a
screenshot.

**Plugin and manufacturer names are user-adjacent too.** They come from a scanner on
somebody else's machine, and D2.4 already found `x86_64-win` leaking into a
manufacturer field. Escape them like everything else.

## SEO

Title, description, Open Graph and Twitter card tags, canonical URL. `noindex` for
link shares. These pages will be pasted into Discord and socials, so the unfurl is
part of the product.

---

# 6. The plugin

Session B built the Chains sidebar with Favourites and Saved, and **Imported designed
for but deliberately not rendered** because nothing could produce one. D3 is what
fills it.

- **Imported section** renders once imports exist. It is a filter on `source='import'`,
  so most of the work is already done.
- **Share from the plugin**: a menu item on a chain row that creates a share and puts
  the URL on the clipboard. The plugin does not need to render the page.
- **Substitution on import**: run the existing logic, show what is missing, offer
  alternatives, and use the standing wording for a plugin that will not authorise.
- **Per-slot restore degradation stays visible**, exactly as Session B proved: a slot
  that will not restore loads at default and says so, naming the plugin. A silent
  default is the worst outcome because it is the only one the user cannot see.

Geometry rules unchanged, and that file has produced four overlap bugs: `resized()` is
the sole author storing rects, `paint()` consumes and measures nothing, visibility is
authored unconditionally with the condition inside the expression, and every
`setVisible` is written per component and never through a pointer loop.

---

# 7. Verification

- A chain whose every field contains `<script>`, quotes, angle brackets and a
  `javascript:` link renders inert on the public page, in the body, the meta tags and
  the OG description. Real parser, raw HTML source shown.
- An archived chain's share 404s.
- A revoked share 404s, and an already-imported copy is unaffected.
- A link share carries `noindex`; a public share does not.
- Import creates a copy with `source='import'` that survives the sender deleting
  theirs.
- Importing your own share is a no-op.
- Imports do not count against the free-tier saved-chain cap.
- The free-tier share cap fires and logs.
- A chain shared to an account missing two plugins reports substitutes **before**
  loading and per-slot outcomes **after**, and never says "not owned".
- **Auth is verified from a real signed-in browser**, not a harness that sets the
  header itself. Any test that constructs auth by hand cannot verify auth: M1 shipped
  a surface where every layer passed while the only path a real user takes was 401.
- Dashboard cold build unchanged: 5 Redis reads, 4 Postgres queries.

---

# 8. Phasing

| Slice | What |
|---|---|
| **D3.0** | Migration 0010, share and import endpoints, caps |
| **D3.1** | The public page at `/c/:slug` |
| **D3.2** | Plugin: share action, Imported section, substitution on import |
| **D3.3** | Dashboard and profile integration: share state on chain rows, public chains on a profile |

D3.3 is where a public profile gains a chains list, which is what makes a profile
worth visiting and closes the loop between D2 and D3.

---

# 9. Gotchas

Carried forward:

- `echojay-saas` is V2. Never `echojay-saas-v2`.
- **One worktree per session.** Four incidents so far.
- No em-dashes anywhere, including comments. Check before committing, not after.
- Raw email never reaches Postgres, a payload, or any HTML.
- Migration before code deploy, always.
- `--plan` and `--status` are read only.
- Redis keys holding Postgres-derived state are environment-scoped via `scopedKey`.
- **Any test that constructs auth by hand cannot verify auth.**
- A scripted edit that fails its assertion has not applied. Verify before continuing.
- `state` and `params` are different things. `getStateInformation` needs no param map.

New with D3:

- **Never call a shared chain a "rack".** `rack-<uid>.json` is the Link sidecar and
  the collision would poison search across both repos.
- **Live while browsing, snapshot on import.** Revoking cannot unimport.
- **Substitution happens in the plugin, never on the web page**, because the web
  version would require storing a full plugin inventory server-side and undo D2's
  privacy decision.
- **Imports never count against a save cap.** Gate outbound reach, never receiving.
- **An archived chain's share 404s**, and that filter is easy to drop in a later query.
- **A chain page describes placement, never result, and never asserts ownership.**
