# EchoJay Wizard server: every error and refusal code (for Kathy, 22 Oct 2026)

Kathy's ruling K8: **our names win.** The wizard stores `reason` (or `error`), shows `message` exactly as sent, and acts only on the codes marked retryable below. Every body is `{"error": "<code>", "message": "<plain English>", …}`, plus `"reason"` on a 422 and `"retry_after_s"` on a 429.

## 1. 422 `rejected`: a result was refused. Never retried (contract §4).

The wizard moves the result to "not accepted", keeps the `reason`, and shows the `message`. `<Plugin>` is the plugin's name from the result.

| `reason` | when | `message` |
|---|---|---|
| `hash_mismatch` | the SHA-256 header doesn't match the decompressed body | "One result arrived damaged, so it wasn't used." |
| `shape` | not JSON, not `ej_wizard_result/1`, an unknown `kind`, a malformed `plugin_id`, a profile with no files, an `outcome` with an unknown `state`, a file that isn't an object, `map.json` with no controls, a tone check that matches no profile | "<Plugin>'s measurements didn't pass our checks, so they weren't used." |
| `result_id` | `run_id` / `result_id` differ from the URL, or `result_id` isn't `res_` + the first 32 hex of SHA-256(`run_id\|kind\|plugin_id`) | "<Plugin>'s results didn't match how they were labelled, so they weren't used." |
| `identity` | a file names another `plugin_id`, or another fingerprint than the result | "<Plugin>'s results don't match the plugin version they say they're from, so they weren't used." |
| `validator` | the served intake's own validator refused a file (`compProfile.js`, the EQ / saturation / draft validators, the tone check's numbers) | "<Plugin>'s measurements didn't pass our checks, so they weren't used." |
| `timestamp` | `created` is more than 5 minutes in the future or more than 5 minutes before the run was created, or `measured_at` is present but isn't a time or is in the future | "<Plugin>'s results had an impossible time on them, so they weren't used." |
| `personal_detail` | a home folder, user name, computer name or email field anywhere in the result | "<Plugin>'s results included a detail about your computer, so they weren't used. Please update the EchoJay Wizard." |

`hash_mismatch`, `result_id` and `identity` also count as tampering in `/complete`, so that run gets no free month (`failed_checks`).

## 2. Every other error

| status | `error` | the wizard may… | `message` |
|---|---|---|---|
| 401 | `token_expired` | **retry**: refresh once, then repeat the call | (not shown) "Your sign-in needs refreshing." |
| 401 | `token_revoked` | not retry: ask the user to sign in again; results stay queued | "You've been signed out. Please sign in again - your results are kept." |
| 400 | `invalid_grant` | not retry: sign in again; results stay queued | "Please sign in again - your results are kept." |
| 401 | `invalid_client` | not retry: the app's client id is unknown | "This version of the EchoJay Wizard isn't recognised. Please update it." |
| 429 | `rate_limited` | **retry** after `retry_after_s` (also sent as `Retry-After`) | "EchoJay is busy - the wizard will carry on sending in a moment." |
| 404 | `run_not_found` | **retry**: `POST /runs`, then repeat the call | (not shown) "That run isn't on the server yet." |
| 409 | `run_conflict` | not retry | "This run belongs to another account. Sign in with the account you started it with." |
| 409 | `result_conflict` | not retry: keep the first one sent, log it | (not shown) "That result was already sent with different contents." |
| 409 | `results_missing` | **retry**: send the listed `missing` results, then call `/complete` again | (not shown) "Some results still need sending." |
| 413 | `too_large` | not retry | "One result was too big to send and wasn't used." · when the run is full: "This run has sent as many results as one run can." · when one file is too big: "One result from <Plugin> was too big to send and wasn't used." |
| 403 | `account_blocked` | not retry | "This account can't send results. Contact support@echojay.ai." |
| 426 | `client_too_old` | not retry: update the wizard | "Please update the EchoJay Wizard - this version can no longer send results." |
| 400 | `run_id_required` | not retry: every `/catalogue/check` must carry `run_id` (ruling K1) | "Please update the EchoJay Wizard - this version can't check your plugins." |
| 400 / 405 | `bad_request` | not retry | "Something in that request wasn't right. Please update the EchoJay Wizard and try again." |
| 404 | `not_found` | not retry now; try again on the next launch (the wizard API is switched off, or the URL is wrong) | "Not found." |
| 500 | `server_error` | **retry** with back-off (5 s, 10, 20 … up to 10 min) and on every launch | "Something went wrong on our side. The wizard will try again." |
| other 5xx, timeout, offline | — | **retry** with the same back-off | — |

## 3. Device sign-in (`POST /auth/token`, RFC 8628), all 400 except where marked

| `error` | the wizard… | `message` |
|---|---|---|
| `authorization_pending` | **keeps polling** at `interval` | — |
| `slow_down` | **keeps polling**, `interval` + 5 s | — |
| `expired_token` | starts the sign-in again | "That sign-in link has expired - try again." |
| `access_denied` | stops: the user cancelled | "Sign-in was cancelled." |
| `invalid_grant` | starts the sign-in again | "Please sign in again - your results are kept." |
| `unsupported_grant_type` | not retry | — |
| `invalid_client` (401) | not retry | "This version of the EchoJay Wizard isn't recognised. Please update it." |
| `rate_limited` (429) | **retry** after `retry_after_s` | "EchoJay is busy - the wizard will carry on sending in a moment." |

## 4. Success replies to `PUT …/results/…`

| status | `status` | meaning |
|---|---|---|
| 201 | `accepted` | passed every check; stored in holding |
| 201 | `held` (**new**, ruling K3) | a file of a schema no validator knows yet: kept in holding, never promoted or served, not counted in `results_accepted`. It carries a `message`: "EchoJay can't check <Plugin>'s measurements yet, so they're kept but not used for now." Mark it sent; don't retry. |
| 200 | `duplicate` | the same id with the same SHA-256 was already received: mark it sent |

## 5. `/complete` counts (ruling K7)

- `results_received`: every result uploaded in the run, a retry counted once (accepted, held and refused).
- `results_accepted`: those that passed validation, any kind (held unknown schemas left out).
- `results_new`: new profiles, i.e. accepted `profile` results for plugins the server answered `need` in this run.
- `plugins_checked`: the run's collection that this account checked under the run (the free-month count).
