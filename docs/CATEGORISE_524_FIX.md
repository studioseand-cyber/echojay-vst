# Categorise times out with a Cloudflare 524 — root cause and the fix

Written 12 Aug 2026. Everything here is measured unless it says otherwise.

## Status — LIVE IN PRODUCTION (13 Aug 2026)

Implemented by Claude Code in `echojay-saas-dialable`: commit `650b855` on branch
`feat/categorise-queue` (cut from `origin/main`). Categorise answers cache hits
as before, returns each miss as `pending` and enqueues it (`categorise:queue`,
deduped by a `categorise:inflight:<key>` NX marker); a new every-minute Vercel
cron drainer (`api/cron-categorise-drain.js`) runs the unchanged two-arm gate in
batches, charges quota once at mint, retries at queue level with an attempts cap,
and honours the kill switch. A `waitUntil` kick drains 2 batches right after the
response so small asks resolve in seconds. Tests: new suite 42/42 (key-scoped
`test:catq:*`, zero prod keys, zero model spend), existing
`test-categories-endpoint.mjs` 14/14 (all 1,073 Mac 1 products still
cache-answered, zero model calls).

**Deployed and verified.** `650b855` fast-forward-merged to `main` and shipped
with `vercel --prod`, aliased to `www.echojay.ai`. Confirmed in production runtime
logs: the per-minute cron `/api/cron-categorise-drain` returns 200 and
authenticates (its own Bearer) every minute; `/api/params/categories` answers in
~0.4 s from the public alias. Branch baseline resolved — `main` is trunk,
`pricing-v2` retired to a tag. The local `categorise.py` workaround is no longer
needed; mappers just press Categorise, on any machine, no keys.

- **Kill switch:** set `config:categorise:disabled` in Redis to halt the drainer
  mid-queue, no deploy needed.
- **Tooling trap:** the installed Vercel CLI (50.41.0) silently drops runtime log
  streams (showed zero requests on a live service — a red herring). Use
  `npx -y vercel@latest logs`, or `npm i -g vercel@latest`.
- **Still to see in the wild:** the first fresh-catalogue mapper run — expected
  shape: a fast 200 with everything `pending`, ~150/min minted in the background,
  fully categorised in under ten minutes, no request near the 100 s window.
- **Optional follow-up:** the app auto-re-asking after a `pending` response
  (ejmap/VST repo) so a mapper doesn't press Categorise twice. Not required.

## Symptom

A mapper presses **Categorise** in the app and, after the wheel spins for a
while, gets **HTTP 524**. Reproduced on two machines (Mac 1 was already fully
categorised so it never sees it; a fresh mapper mac — Carl's — hit it every
time). The failure is identical across machines, which is the first and most
important clue: two different macs do not fail the same way by coincidence.

## Root cause — one level up from the mac

`524` is a **Cloudflare** status: the CDN opened a connection to the origin but
the origin did not finish responding inside Cloudflare's ~100‑second window.
So this is not the mapper's machine, not auth, not a wrong path. Things that
were ruled out, not assumed:

- **Endpoint override.** `~/.echojay/dev.json` was absent on the failing mac
  (`no dev.json — using default endpoint`), so it is hitting the real
  production origin, not a stale preview build.
- **The machine / credentials.** The mapper mac has no `ANTHROPIC_API_KEY` /
  `OPENAI_API_KEY` (by design — the server holds them), so nothing on the mac
  is doing the AI work. The timeout is entirely origin‑side.

What the endpoint does today: `POST /api/params/categorise` collapses the scan
census to products, answers the cached ones, and then runs the two‑arm AI gate
over **every unknown product synchronously, inside the one request**.
`SERVER_CONTRACT.md §2.1` measures that pass at **1,073 products, ~6 minutes**.
Six minutes is far past Cloudflare's ~100 s, so any first run over a catalogue
with a meaningful number of new products is killed with a 524 before it can
answer. Vercel's own 300 s function ceiling (§8) is irrelevant — Cloudflare
cuts it first.

So the visible failure is "categorise failed on a mac," but the defect is
architectural: **long synchronous work behind a CDN timeout.** Retrying,
raising a client timeout, or bumping Vercel `maxDuration` do not touch it —
Cloudflare is the binding limit and the origin genuinely takes minutes.

**Not the fix: putting the API keys in the app.** That would spread Sean's
billable keys onto every mapper machine (and into anything that can read the
app's config), which is exactly what the server‑holds‑the‑keys design exists to
prevent and what §10 of the handover is already anxious about. The keys are in
the right place. Nothing in the mapper software needs to change.

## The fix — make the endpoint non‑blocking (its own contract already says so)

`SERVER_CONTRACT.md §2.1` already specifies the target behaviour:

> **Does**: collapse to products, answer from cache, queue the unknown
> remainder for the two‑arm call. An unanswered product returns
> `"disposition":"pending"` … it resolves on a later refresh.

Implement that. It is the same Upstash Redis queue + Vercel drainer shape
already scoped for the arrival path (handover §8), confirmed viable without
Modal.

### 1. `POST /api/params/categorise` returns fast, never blocks

1. Collapse census rows to products by product key `format|uid` (version
   dropped — per handover §4 keying), as today.
2. Answer every **cached** product from the store immediately.
3. For each **unknown** product: push its product key onto an Upstash Redis
   queue (`categorise:queue`), guarded by a per‑product marker
   (`categorise:inflight:<key>`) so concurrent requests and cron ticks never
   double‑enqueue, and return it as `"disposition":"pending"`.
4. **Never** return `"sweep"` with an empty category — that is a contract
   violation (§2.1), not a state. `pending` is the only correct answer for a
   not‑yet‑categorised product.

Response cost is now cache lookups + a Redis push — milliseconds, independent
of how many products are unknown. It cannot approach the 100 s limit.

### 2. A background drainer does the AI work

A Vercel **cron** (e.g. every minute) or an enqueue‑triggered background
function pops a small batch — `N = 10–25` product keys — and runs the **same
two‑arm gate the server already ports from `tools/propose/categorise.py`
unchanged** (agree + both confident → `sweep`; agree + one hedged → `sweep`,
`confidence:"hedged"`; agree `none` → `no_dial_set`; agree `not_a_processor` →
`not_a_processor`; else `review`). `N` is chosen so one batch's model calls
finish well inside the function limit; each invocation is therefore always
inside the 300 s ceiling with margin.

- Write each result to the category cache, then clear its `inflight` marker and
  remove it from the queue.
- **Retry at the queue level.** A failed arm writes nothing (mirroring
  `categorise.py`: presence means done), so a transient model outage just
  leaves the product on the queue for the next tick. No product is recorded as
  a permanent failure because one call timed out.
- **Idempotent.** Skip anything already cached; the `inflight` marker plus the
  presence check prevent double‑enqueue and double‑charge.

### 3. Client — little or no change

The sweep's `decideSweep` already skips an uncategorised product
(`skipped_uncategorised`) rather than erroring, and the contract already frames
`pending` as "resolves on a later refresh." Confirm the app re‑requests
categorise (or a lightweight status read) a short time after a run that
returned `pending`, so newly cached categories are picked up. If it does not
already, adding that refresh is the **only** possible client‑side change, and
it is minor. A mapper can begin sweeping the already‑categorised set while the
remainder resolves in the background.

## Why this is safe

- Two‑arm logic is unchanged; `categorise.py --selftest` (40 hand answers, 103
  both‑confident agreements) still governs correctness.
- Keys stay server‑side. Mapper software distributes no new secrets.
- Partial/pending states degrade gracefully — the sweep already tolerates
  uncategorised products, so nothing breaks while the queue drains.
- Same infrastructure (Upstash Redis + Vercel cron) already validated for §8.

## Acceptance criteria

1. A categorise POST with `M` unknown products (test `M ≈ 1,000`) returns in
   **< ~2 s** with those `M` as `pending`, no 524, for any `M`.
2. The endpoint never returns `sweep` with an empty category (§2.1 invariant).
3. The background worker categorises all pending products within a bounded
   number of ticks; **no single invocation exceeds the function limit.**
4. Re‑POST after the queue drains returns everything from cache, zero pending.
5. No product is categorised twice; per‑product cost ≈ $0.0028, unchanged.
6. A first‑run 1,819‑row census (§2.1) fully categorises in the background with
   **no request ever near the Cloudflare ~100 s limit.**

## Rollout

Backward‑compatible: an older client that does not poll still gets cached
answers on its next manual Categorise. After deploy, watch the Vercel logs for
`/api/params/categorise` — it should show fast 200s, with the model work
appearing separately under the cron/drainer.

## Interim, until this ships

Two unblocks for a mapper hitting new plugins, both already used:

- Run `tools/propose/categorise.py` locally on a **trusted, keyed** machine
  (writes `~/Library/ejmap/categories.json` directly; what was done for Carl's
  mac on 12 Aug — one file + `pip install anthropic openai` + the keys set for
  one shell session, removed after).
- Sean pre‑warms the server cache for the new products.

Neither scales past a mapper or two, which is the whole reason to build the
queue.
