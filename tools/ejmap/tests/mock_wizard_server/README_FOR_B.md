# The EchoJay Wizard's mock server, fixtures and replays (contract v1.1)

This package goes with `WIZARD_SERVER_CONTRACT_v1.1.md` (the changelog at its top lists every change since v1). Everything in it was produced on our side against our own mock; nothing came from a real server.

**What is new on 31 Oct (the measuring minimums; the contract's changelog, "31 Oct"):**
- **The mock now follows staging's rule:** each category's minimum measuring time is a third of the fastest genuine time, at least 1 s.
- **Staging's own figures:** compressor 50 s, channel strip 1 s, limiter (ceiling only) 1 s. A limiter that also sends its compressor profile takes the compressor's 50.
- **Please confirm the others:** we derived them with the same rule from the 19-23 Oct times (the rule reproduces your compressor 50 exactly): limiter 50, EQ 2, saturation 1, de-esser 4, reverb 4, delay 6, filter 1, gate 7, transient shaper 4, pitch 2, amp sim 1.
- **The fixtures were recorded again** against these minimums. Meaw Chain's 2.97 s strip is now accepted, as it was by staging on 29 Oct.

**What was new for STAGING 7 (29 Oct evening; the contract's changelog, "29 Oct, evening", and §4e):**
- **Three optional keys in a result's `payload`:** `display_check` (`ej_display_check/1`, per control: did the display tell the truth about the sound), `binary` (`{sha256, bytes}` of the plugin's executable) and `readings` (`ej_readings/1`, a compact copy of the raw readings, or `{withheld}`).
- **The mock** checks each one when it is there (`collected_problem`: a malformed one is 422 `shape`) and counts what arrived in `/__mock/state`'s `collected`.
- **Please confirm** that staging keeps a result carrying them: stored, not refused, not held.
- The recorded scenarios and `18_real_results` are unchanged (they were recorded before these keys).

**What was new in the 29 Oct package (the staging rehearsal; the contract's changelog, "29 Oct"):**
- **Two more outcome states: `bridged` and `bridge_no_match`** (a VST3 read against its AU twin; the server decides what it inherits). The mock accepts both; new scenario `13f_vst3_twin_outcomes` sends one of each.
- **Only a profile carries a measuring span.** A reference vote, a bridge read and an outcome start at their finish (§4 always said so). In `18_real_results`, 2 documents change: a reference vote and an outcome.
- **No new numbers elsewhere:** 18_real_results still sends 33 results (32 accepted, 2 of them as held drafts; 1 held).

**What was new in the 27 Oct package (your notes; the contract's changelog, "27 Oct"):**
- **Limiter and de-esser drafts are validated,** as your server does: accepted, 201 `stored: "held_draft"`, counted in `results_accepted`. A limiter draft with no `ceiling` block, or a de-esser draft with no `deesser` block, is 422 `validator`. `ej_readback_sweep/1` stays held.
- **A REAL limiter ceiling draft:** L2 (m), measured on this Mac (its ceiling holds true peak). `18_real_results` now sends **33** results: 32 accepted (the de-esser and limiter drafts as held drafts), 1 held (the bridge read).
- **The pitch draft's key** is `pitch_measurement` (was `measurement`, which every measured file's identity block uses).
- **The stand-in data:** every stand-in fingerprint is 64 hex characters, and every stand-in curve stays below the sweep's -3.01 dBFS ceiling.

**What was new in the 24 Oct package (your answers on v1.1; the contract's changelog, "24 Oct"):**
- **The two draft ids:** `ej_limiter_profile/1` and `ej_deesser_profile/1` (were sentences). The one real de-esser draft in `18_real_results` carries its new id; nothing else in it changed.
- **Held, as your server held them then** until you had their validators: `ej_limiter_profile/1`, `ej_deesser_profile/1`, `ej_readback_sweep/1` (any result kind). (27 Oct: only `ej_readback_sweep/1` is still held.)
- **Another plugin's `map.json`** is 422 `shape` (was `identity` in our mock).
- **A "need" plugin whose only results are a reference vote or a bridge read** now gets its outcome too: `18_real_results` sends **32** results (was 30).

**What was new in the 22 Oct package:**
- **The mock is v1.1:** your reason codes (names, statuses, retryable or not, your words), the `held` status, the `/complete` counts, `started_at` on POST /runs, required `measured_at`, the server's queue of "need" answers, the promo refusal order, compressor agreement (ruling 10) and `expires_at`.
- **The fixtures were recorded again** against it, from the real wizard client. Scenario `18_real_results` sends **30 REAL results** from the 20 Oct wizard run (one Mac): 12 compressor profiles with their tone checks, plus one real result of every other kind. Their `measured_at` are the run's own times; everything else as the engine packed it.
- **Your `wizard-deep-replay.py` is included unchanged**, beside our `replay.py`. Both give 0 differences against this mock with `--shift-created`.

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
| `POST /__mock/config` | server config (`qualify_minimum`, `collection_threshold`, `collection_min_size`, `agree_tolerance`, `force_429_once`; 31 Oct: `catalogue_delays_s` (one delay per check call, in order) and `catalogue_delay_s` (every call after), for a slow answer; 28 Oct: `finish_run_after_results` N, the run finished on the server once N results are in) |
| `POST /__mock/complete_run` `{"run_id": …}` | 31 Oct: a run finished on the server mid-upload (rehearsals). From then on its checks and results answer 409 `run_completed` |
| `POST /__mock/catalogue` | seeding what the catalogue already has |
| `POST /__mock/verify` `{"plugin_id": …}` | our own verification certifying a plugin |
| `POST /__mock/age_check` `{"run_id": …, "minutes": n}` | v1.1: the run's first catalogue check happened n minutes ago. Used only by our suite's offline timeline, which is not in the fixtures (§3) |
| `POST /__mock/agree` `{"a": …, "b": …}` | v1.1: would these two compressor profiles certify each other (ruling 10)? |
| `POST /__mock/scenario`, `GET /__mock/state`, `POST /__mock/tokens_known` | labelling recordings, and reading the mock's state in tests |

**Mock test accounts:** `acct_mock_a` … `acct_mock_g`. The mock's validator is a stand-in: it checks the envelope, the times, the full map and the schema list (an unknown schema is `held`). Yours is the served intake's own (compProfile.js and its siblings).

## 2. The fixtures

`fixtures/<scenario>/<seq>.json` holds one exchange per file: the request the real wizard client made (method, path, headers, body) and the mock's answer (status and body). `seq` is the global order: the scenarios share accounts and state, so replay them in order.

**Placeholders.** Tokens, device codes, user codes and promo codes are replaced by `<ACCESS_TOKEN_n>`, `<REFRESH_TOKEN_n>`, `<DEVICE_CODE_n>`, `<USER_CODE_n>` and `<PROMO_CODE_n>`. The same value always gets the same placeholder. Nothing else is altered.

**Result bodies.** A result's body is recorded decoded (`gzip_json`). On the wire it is gzipped, with `X-EJ-Content-SHA256` set to the SHA-256 of the uncompressed body. `sha_header_matched: false` marks the one upload that deliberately sent a wrong SHA-256 (the cheat-check scenario).

| scenario | what it shows |
|---|---|
| 00_setup | the mock's config and catalogue (test hooks) |
| 01_happy_path | device sign-in, the catalogue check (`run_id` required), the run (with the wizard's `started_at`), results |
| 02_offline_then_retry | the result sent after the wizard was offline. **The offline attempts never reached a server, so they cannot be recorded**; the client keeps the result and retries with back-off (5 s, doubling to 10 min) |
| 03_duplicate_upload | the same result sent again: `200 duplicate` |
| 04_rejected_upload | a result the validator refuses (`422`, your reason code and words), and one with an unknown schema: `201 held` (kept, never promoted or served, not counted as accepted) |
| 04b_full_map | every result carries the plugin's full map: one accepted; a parameter list, and another plugin's map, refused (`shape`) |
| 05_rate_limited | `429 rate_limited` with `retry_after_s`, then the same result accepted |
| 06_results_missing | `/complete` naming a result the server does not have: `409 results_missing` |
| 07_happy_path_end_with_promo | the run's end: the counts (received / accepted / new / checked) and the promo issued, with `expires_at` |
| 08_promo_asked_twice | the same run asking again: the same code, `issued_now: false` |
| 09_not_eligible_account_already_claimed | a second run on the same account: no code (`account_already_claimed`) |
| 10_same_collection_other_account | another account with a near-identical plugin collection: no code (`collection_already_claimed`) and its result cannot certify |
| 11_second_tester_certifies | another account and collection: a code; its agreeing result certifies, a disagreeing one stays unverified; our own verification |
| 12_below_minimum_then_met_by_checks | `below_minimum`; the queue is every plugin the server answered "need", and the wizard sends an outcome for each; a check under another run_id doesn't count |
| 13_failed_cheat_check | a tampered upload (wrong SHA-256), then no code (`failed_checks`) |
| 13b_offline_before_run | v1.1: the tester was offline from before POST /runs; on reconnect the run is created with the wizard's own `started_at` and **every result is accepted**. Two controls that hold at any replay time: a missing `measured_at` (`shape`) and one ten years ahead (`timestamp`) |
| 13d_four_at_once | v1.1 (24 Oct): `measure_started_at` - four compressor profiles measured at once, **all accepted**; then a fifth at once, a compressor measured in 30 s, a start after its end (`timestamp` each) and a missing start (`shape`); an outcome whose start is its end, accepted. Every time is relative, so either replay keeps each verdict |
| 13e_measurement_identity | v1.1 (24 Oct): every measured file of a profile carries `measurement` {run_id, result_id, nonce, measure_started_at, measured_at}. One honest profile, accepted; then another run's identity, only its run edited, another result's id and a reused nonce (`identity`, tampering), and no block (`shape`) |
| 13f_vst3_twin_outcomes | 29 Oct: a VST3 twin's two outcome states, `bridged` and `bridge_no_match`, each accepted |
| 13h_run_completed | 31 Oct (B, 27 Oct): a check, `POST /runs` and `/complete`; then a check and a result under that finished run, each 409 `run_completed` with B's words; then a check under a new run id, answered as normal |
| 13c_promo_refusal_order | v1.1: two refusals at once answer with the first in the agreed order: account > collection > not finished > failed checks > below minimum |
| 14_expired_token_refreshed | `401 token_expired`, the refresh (rotated), the call repeated |
| 15_refresh_token_reused | a used refresh token presented again: `invalid_grant`, the whole family revoked (`401 token_revoked`) |
| 16_sign_in_denied | the user denies the sign-in: `access_denied` |
| 18_real_results | the real results (above), 33 with the two outcomes and the L2 limiter draft: 32 accepted (the de-esser and limiter drafts as held drafts), 1 HELD by design (the bridge read), none refused |

## 3. Replay against any base URL

```
python3 -I wizard-deep-replay.py --base http://127.0.0.1:8787 --fixtures fixtures --shift-created    # yours: field by field
python3 replay.py --base http://127.0.0.1:8787 --mock --shift-created                                # ours: status + error/status/reason
python3 replay.py --base https://<your staging> --client-id <yours> --shift-created                  # ours, against yours (stops at each test hook)
```
- **`--shift-created`:** your script moves each result's `created` so the earliest is a minute ago, keeping the gaps. Ours moves every time the wizard sent (`created`, `measured_at`, `measure_started_at`, `started_at`) by that same amount.
- **Placeholders:** both bind each one to the real value your server returns the first time it appears, and use it in the later requests.
- **Test hooks against your server:** ours stops at each one and says what to do on your side, then carries on when you press Return.
- **On our mock, both give 0 differences** (run on 22 Oct against a fresh mock).
- **What a replay can't carry: an offline timeline.** A replay moves the recorded times to replay time and sends POST /runs live, so the gap between "packed" and "the run record arrived" collapses. The recorded 13b keeps its gaps inside the 5-minute allowance. The long timeline (packed 20 min before the run record: accepted; before `started_at`, before the first check, `measured_at` an hour ahead: refused) is pinned in our own suite and worked through in the contract, §4.

## 4. Result sizes

`result_sizes.md` / `result_sizes.json` give min / median / 95th / max, raw and gzipped, per result type, from one real wizard run (20 Oct 2026). They predate the full map in every result: add the map's size (the real results in `18_real_results` show it).
