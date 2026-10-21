# The EchoJay Wizard's mock server, fixtures and replay (contract v1)

This package goes with `WIZARD_SERVER_CONTRACT_v1.md`. Everything in it was produced on our side against our own mock; nothing came from a real server.

## 1. Run the mock

```
python3 mock_server.py --port 8787          # http://127.0.0.1:8787/api/wizard/v1/...
```
- Python 3 standard library only. It listens on 127.0.0.1 only.
- The mock's public client id is `ejwiz-mock`.
- `--port 0` picks a free port; `--port-file F` writes the chosen port to F.
- `--record DIR` writes every exchange to DIR. These recordings are **raw**, with real (mock) tokens; this package's fixtures were scrubbed from such a recording.

**Test hooks only. These are NOT part of the contract; please don't build them:**

| hook | what it stands in for |
|---|---|
| `POST /__mock/approve` `{"account_id": …}` | the user approving the sign-in in the browser |
| `POST /__mock/deny` | the user denying it |
| `POST /__mock/expire_access` | access tokens expiring |
| `POST /__mock/config` | server config (`qualify_minimum`, `collection_threshold`, `collection_min_size`, `agree_tolerance`, `force_429_once`) |
| `POST /__mock/catalogue` | seeding what the catalogue already has |
| `POST /__mock/verify` `{"plugin_id": …}` | our own verification certifying a plugin |
| `POST /__mock/scenario`, `GET /__mock/state` | labelling recordings, and reading the mock's state in tests |

**Mock test accounts:** `acct_mock_a` … `acct_mock_d`. The mock's validator is a stand-in. Yours is the served intake's own (compProfile.js and its siblings).

## 2. The fixtures

`fixtures/<scenario>/<seq>.json` holds one exchange per file: the request the real wizard client made (method, path, headers, body) and the mock's answer (status and body). `seq` is the global order: the scenarios share accounts and state, so replay them in order.

**Placeholders.** Tokens, device codes, user codes and promo codes are replaced by `<ACCESS_TOKEN_n>`, `<REFRESH_TOKEN_n>`, `<DEVICE_CODE_n>`, `<USER_CODE_n>` and `<PROMO_CODE_n>`. The same value always gets the same placeholder. Nothing else is altered.

**Result bodies.** A result's body is recorded decoded (`gzip_json`). On the wire it is gzipped, with `X-EJ-Content-SHA256` set to the SHA-256 of the uncompressed body. `sha_header_matched: false` marks the one upload that deliberately sent a wrong SHA-256 (the cheat-check scenario).

| scenario | what it shows |
|---|---|
| 00_setup | the mock's config and catalogue (test hooks) |
| 01_happy_path | device sign-in, the catalogue check (with its run_id), the run, results |
| 02_offline_then_retry | the result sent after the wizard was offline. **The offline attempts never reached a server, so they cannot be recorded**; the client keeps the result and retries with back-off |
| 03_duplicate_upload | the same result sent again: `200 duplicate` |
| 04_rejected_upload | a result the validator refuses: `422 rejected`, with plain words |
| 05_rate_limited | `429 rate_limited` with `retry_after_s`, then the same result accepted |
| 06_results_missing | `/complete` naming a result the server does not have: `409 results_missing` |
| 07_happy_path_end_with_promo | the run's end: the promo issued |
| 08_promo_asked_twice | the same run asking again: the same code, `issued_now: false` |
| 09_not_eligible_account_already_claimed | a second run on the same account: no code (`account_already_claimed`) |
| 10_same_collection_other_account | another account with a near-identical plugin collection: no code (`collection_already_claimed`, "The free month has already been claimed for this setup.") and its result cannot certify |
| 11_second_tester_certifies | another account and collection: a code; its agreeing result certifies, a disagreeing one stays unverified; our own verification |
| 12_below_minimum_then_met_by_checks | `below_minimum` ("this run checked 1"); a check under another run_id doesn't count; then the same run, with no uploads, earns a code once enough plugins were checked under it |
| 13_failed_cheat_check | a tampered upload (wrong SHA-256), then no code (`failed_checks`) |
| 14_expired_token_refreshed | `401 token_expired`, the refresh (rotated), the call repeated |
| 15_refresh_token_reused | a used refresh token presented again: `invalid_grant`, the whole family revoked (`401 token_revoked`) |
| 16_sign_in_denied | the user denies the sign-in: `access_denied` |

## 3. Replay against any base URL

```
python3 replay.py --base http://127.0.0.1:8787 --mock                      # against our mock (sends the test hooks)
python3 replay.py --base https://<your staging> --client-id <yours>         # against yours
python3 replay.py --base … --scenario 03_duplicate_upload                    # one scenario
```
- The replay sends each recorded request in order. It compares the status, plus the `error` / `status` / `reason` fields and the promo's `eligible` / `reason`.
- It binds each placeholder to the real value your server returns the first time it appears, and uses it in the later requests.
- **At each test hook against your server, it stops and says what to do on your side** (e.g. "approve the waiting sign-in in the browser as test account acct_mock_b"), then carries on when you press Return.
- Exit 0 = every exchange matched.

## 4. Result sizes

`result_sizes.md` / `result_sizes.json` give min / median / 95th / max, raw and gzipped, per result type, from one real wizard run (20 Oct 2026). Use them to size the request limit, storage and the decompression cap.

