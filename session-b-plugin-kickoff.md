# Session B, plugin half: how to talk to the backend

Written 28 Jul 2026, after the backend half merged and was proven end to end
against a preview deployment. Everything in here was verified live, not
assumed.

Repo for this session: `~/Documents/ECHOJAY FILES/ECHOJAY VST/echojay-vst-v200`.
The backend half is merged to `main` in `echojay-saas` and is NOT deployed to
production.

---

## The one sentence version

Point the plugin at a **preview deployment URL** held in local config, send
an `x-vercel-protection-bypass` header on every request, and authenticate
with the **normal EchoJay bearer token** you already have. All three are
needed; missing any one looks like a different failure.

---

## 1. Base URL: config, never hardcoded

Preview deployments get a new URL on **every single deploy**:

```
https://echojay-saas-8v3cw92w9-studioseand-4399s-projects.vercel.app
```

That hash changes each time anyone deploys, including from the backend
session. There is no stable preview alias today. A hardcoded URL will work
for exactly one afternoon and then fail in a way that looks like the backend
is down.

**Read it from local config.** Something like `~/.echojay/dev.json`, outside
the repo so it cannot be committed:

```json
{
  "baseUrl": "https://echojay-saas-8v3cw92w9-studioseand-4399s-projects.vercel.app",
  "protectionBypass": "<secret, see section 3>"
}
```

Production is `https://www.echojay.ai` and needs no bypass header, but every
`/api/v2/*` route there answers `404 {"error":"not_enabled"}` until the
dashboard flag is flipped. Developing against production is not an option
yet, which is the whole reason for this document.

---

## 2. Auth: your existing token works, unchanged

**`JWT_SECRET` is identical across Development, Preview and Production**, and
all three share one Upstash Redis, so accounts, sessions and workspace blobs
are the same everywhere. Verified live: a token minted by
`POST https://www.echojay.ai/api/signup` authenticated successfully against
a preview deployment and returned real data.

Practically:

- Log in or pair however you already do, against production.
- Send the token to the preview as `Authorization: Bearer <token>`.
- No separate preview account, no re-login, no test credentials.

Device pairing works too, with one wrinkle: the **browser** leg
(`/api/device/authorise`) passes Vercel SSO on its own because you are signed
into Vercel in that browser, but the **plugin's** calls (`/api/device/start`,
`/api/device/poll`) need the bypass header like every other request.

---

## 3. The bypass header, and why you need it

Preview deployments sit behind Vercel Authentication. Without the header a
preview URL answers **302 to `vercel.com/sso-api`**, which a plugin cannot
complete because it is not a browser. You will see a redirect where you
expected JSON.

Send this on **every** request to a preview:

```
x-vercel-protection-bypass: <secret>
```

A secret already exists on the project (scope `automation-bypass`). Get it
with `vercel project protection` in the backend repo, or from Project
Settings, Deployment Protection.

Verified behaviour on the current preview:

| Request | Result |
|---|---|
| no header | `302` to Vercel SSO |
| header, no token | `401 {"error":"unauthenticated"}` |
| header + production token | `200` |

That middle row is the useful diagnostic. **`401` means the bypass worked**
and only auth is missing. `404 not_enabled` would mean the dashboard flag is
off. `302` means the header is missing or wrong. Three different failures,
three different codes.

Optional: add `x-vercel-set-bypass-cookie: true` and the response sets a
cookie so later calls on the same connection skip the check. Not required.

---

## 4. Keeping the secret out of a release build

The bypass secret grants access to every protected deployment on the project.
It must never ship. A grep in the build script is not enough, because a grep
only catches what someone remembered to look for.

**Make it structurally impossible: compile-time exclusion.**

Put the whole development transport behind a preprocessor guard so the code
that reads the config file, the code that sets the header, and the strings
themselves are **not in the release binary at all**:

```cpp
#if ECHOJAY_DEV_TRANSPORT
    // Reads ~/.echojay/dev.json, applies baseUrl override and the bypass
    // header. Compiled ONLY in Debug.
    applyDevTransport (request);
#endif
```

Define `ECHOJAY_DEV_TRANSPORT=1` in the Debug configuration only, never in
Release. Then:

- the header name and the config path are not compiled in, so they cannot be
  extracted from the binary
- `baseUrl` cannot be overridden in Release, so a shipped plugin can only
  ever talk to `https://www.echojay.ai`
- forgetting to remove a debug line is harmless, because the compiler removes
  the whole block

**Verify it rather than trusting it**, as a release step:

```sh
strings EchoJay.vst3/Contents/MacOS/EchoJay | grep -i "protection-bypass" && echo FAIL || echo clean
strings EchoJay.vst3/Contents/MacOS/EchoJay | grep -i "vercel.app" && echo FAIL || echo clean
```

Both must print `clean`. This checks the artefact, not the source, which is
the only check that actually answers the question.

Also: `~/.echojay/dev.json` lives outside the repo, so it cannot be committed
by accident, and the secret never enters git history.

---

## 5. The endpoints

All require the bearer token, and on a preview also the bypass header.

```
GET   /api/v2/chains        -> { chains: [...] }   list, NEVER returns state
GET   /api/v2/chains/:id    -> { chain }           full chain INCLUDING state
POST  /api/v2/chains        -> 201 { chain }       create
PATCH /api/v2/chains/:id    -> 200 { chain }       Save, overwrites an existing chain
```

**Create and Save**

```jsonc
{
  "name": "My chain",
  "projectId": null,            // optional, must be a project you own
  "source": "plugin",
  "slots": [
    { "n": 1, "plugin": "Renaissance Vox", "manufacturer": "Waves",
      "format": "VST3", "version": "14.0", "uid": "1096303697",
      "bypassed": false, "role": "level control", "params": null }
  ],
  "state": { "1": "<base64>", "2": null }
}
```

`n` is **1-based and contiguous** and is rejected otherwise. `params` must be
`null` or absent until Phase 2. `state` is keyed by the 1-based slot number,
values base64 or `null`, and `null` means "did not capture" rather than
"captured nothing".

**Plugin version lives in `slots[n].version` only.** Do not duplicate it into
`state`: one fact, one owner, and `state` stays purely opaque. Restore reads
the version from the slot.

**Caps, enforced server side on DECODED bytes**, matching what
`getStateInformation` hands you: 256KB per slot, 1MB total. The server
**rejects** rather than truncating, so cap on your side first and degrade
honestly, storing `null` for the slots you skipped and telling the user which
plugin did not capture.

**Responses never echo state back.** Create and Patch return `hasState`, a
boolean. Only `GET /api/v2/chains/:id` returns the blob itself.

---

## 6. Status codes you will actually hit

| Code | Meaning | What to do |
|---|---|---|
| `302` | Vercel SSO | bypass header missing or wrong |
| `401` | unauthenticated | bearer token missing or expired |
| `404` `not_enabled` | dashboard flag off | you are hitting production, not a preview |
| `404` `not_found` | unknown chain, or not yours | ownership is enforced; same answer either way |
| `400` | bad slots, bad state shape, bad project | message names the problem |
| `402` `chain_limit_reached` | free tier at 10 saved chains | show the upgrade path, do not retry |
| `413` `state_slot_too_large` | over 256KB decoded for one slot | null that slot and say which plugin |
| `413` `state_total_too_large` | over 1MB decoded total | keep the smallest, drop largest first |
| `503` | backend unavailable | retry, do not lose the user's chain |

Every error body is `{ "error": "<code>", "message": "<human sentence>" }`.

---

## 7. What the preview is wired to, so results make sense

Since 28 Jul, preview and production are **no longer the same database**:

| | Production | Preview |
|---|---|---|
| Postgres | production Neon | **d0-dash branch** (has migration 0006) |
| Redis | shared | **shared** |
| `JWT_SECRET` | shared | **shared**, deliberately |
| `DASHBOARD_ENABLED` | absent, so dark | `true` |

Consequences worth knowing:

- Chains you save against a preview land in the **dash branch** and will not
  appear in production. That is intended.
- Redis is still shared, so **a blob save against a preview overwrites the
  real production workspace for that account**. Use a throwaway account for
  anything that writes `data:{email}`, not your own. This is the one
  remaining cross-environment hazard and it is documented rather than fixed,
  because creating a second Upstash database needs console access.
- Postgres-derived Redis keys (`dash:payload:*`, `dashsync:*`) are namespaced
  per environment, so preview cannot poison production's dashboard cache or
  suppress its reconcile writes.

---

## 8. Backend work that is done, and what is not

Done and merged: migration 0006 (`state jsonb`), all four verbs, state
validation with the caps, the free tier cap of 10 saved chains, and the
onboarding step relabelled to "Save a chain".

**Not done, and it is plugin-side:** B.0, stamping chat `updatedAt` on the
plugin's save path. D1 stamps it from the web only, so a plugin save drops
the field the web wrote and those chats fall back to creation order in
`recentChats`. Preserve any existing `updatedAt` on chats the plugin did not
touch, and remember both clients send `chats` and `albums` whole, so unknown
fields must not be dropped.

`onboarding.builtChain` needs no backend change. It already reads live from
`dash.chains`, so it flips to true on the first real save.

**Migration 0006 is deliberately NOT applied to production.** It goes with
the production deploy when the plugin half is ready, so nothing sits half
applied. Preview has it, which is why preview development works today.
