# The stranger's-Mac test — pass criteria (written 2 Oct 2026, before the run)

The aim is not another profile. It is the EJ Map certification PROCESS working unattended on a Mac
we do not control, with plugins we did not choose. Test bed: Sean's Mac — a different library
(Waves 15), no iLok, his Developer ID — run by Kathy exactly as a mapper would. The run is judged
against this page, item by item, not explained afterwards. Each criterion says what is measured and
what counts; "pass" is binary per item.

A dress rehearsal on the operator's Mac (empty ledger, empty store, the packaged app, a small
licence-free slice) is scored against the same items first (section C).

## A. Criteria

| # | criterion | measured by | pass |
|---|---|---|---|
| A1 | **Installs from the packaged app alone.** No repo, no build tree, no hand-edited path. The probe is found by default (inside `ejmap.app/Contents/MacOS`). | The runbook's mapper steps name only `ejmap.app` and `~/Library/ejmap`. `--cert-sweep-all` is run with NO `--probe`, `--fixtures` or `--out`; `pre-flight` prints the probe it found and its Team Identifier. | The pre-flight line names the probe inside the app bundle, signed with the operator's Team ID; no step in the mapper runbook references a path outside the app or `~/Library/ejmap`. |
| A2 | **Scan + categorise + census complete, counts recorded.** | `cert/census.txt` written by the batch's own first step: installed AUs, discovered by category, held by reason, runnable count. | The file exists, every count is a number, and `runnable + held + not discovered = installed compressors+tuners` reconciles. |
| A3 | **Every discovered compressor and tuner ends in exactly ONE named state.** States: `exported` / `refused` (stage + reason) / `held` (licence or hardware, which) / `needs_review` (why). | `cert/outcomes.json`: one row per discovered product, with `state` and `reason`; the batch's own closing summary counts them. | Rows == discovered products (no silent drop); every row has exactly one state; the four counts sum to the row count. |
| A4 | **No single hang stalls the batch.** | Per-process timeout (`--timeout-s`, default 240 s) and the per-product unclean budget (2); a product that times out is `refused` at stage `budget`, and the batch moves on. The batch log carries wall time per product. | The batch reaches its closing summary; no product's wall time exceeds the per-product ceiling (processes × timeout + 60 s); every timed-out product has a row. |
| A5 | **Every exported profile carries its tone-check result.** | The exported `ej_comp_profile/1` file has `tone_check {L_rms_dbfs, g_db, gr_measured_db, pass_within_0_5_db, writes[]}` embedded, and the `.tonecheck.json` sits beside it. | Every `exported` row's file has the block; a profile whose tone check could not run is not `exported` (it is `needs_review` with the reason). |
| A6 | **Nothing sent to the map store; results come back as one zip of `~/Library/ejmap/cert`.** | The runbook never names Send; the batch never calls the upload path (grep the batch log for `send`/`upload`: none); the handover is `zip -r cert.zip ~/Library/ejmap/cert`. | The map-store's row count for the operator's token is unchanged before/after (operator checks); the zip contains `cert/` only — no `config.json`, no token. |
| A7 | **ZERO hand steps.** No per-plugin flags, no candidate picks, no hand re-runs. | The mapper runbook's command list, verbatim; every product-specific decision is made by a rule in the binary and recorded on the product's row. | The runbook contains no `--product`, `--candidate`, `--retry-refused`, `--include-pace` or per-plugin instruction; a second invocation of the batch (resume) changes nothing but completes what was unfinished. |
| A8 | **Resumable.** A batch stopped at any point (power, sleep, ctrl-C) continues from where it was on the next invocation without re-measuring finished products. | Stop the rehearsal batch mid-way once; re-run the same command. | Finished products keep their rows and files (same timestamps); unfinished ones complete; no duplicate rows. |
| A9 | **Tuners end in a state too.** Every discovered tuner has a row: `recorded` (the pitch record written) counts as its pass state, else refused / held / needs_review. | Same `outcomes.json`. | As A3, for the tuner category. |

## B. What is NOT a pass criterion (so nobody argues it later)

- How many products export. A library of refusals with honest reasons passes; one silent drop fails.
- Whether a tone check passes. A failed tone check is recorded on an `exported` profile; it is a
  result, not a defect of the process.
- Channel strips. Until the candidate rule (item 2 of the 2 Oct plan) lands, a product with several
  threshold candidates ends as `needs_review` with the candidate list — a pass state.
- PACE products on a Mac without the iLok: `held (licence)` — a pass state. The batch must not try
  them (it would fill the store with "dongle absent" refusals): the census's licence classification
  decides, and the iLok's absence is logged at the start of the batch.

## C. The dress rehearsal (operator's Mac, before Sean's)

Same criteria, scored on: an EMPTY scratch ledger (`--ejmap-ledger` on a fresh directory), an EMPTY
store (`--out` on a fresh directory, so `fixtures/` starts empty), the packaged `ejmap.app` with the
probe inside, the stranger's-Mac runbook followed verbatim, a slice of 5–10 licence-free products that
includes one channel strip and one tuner. The slice is the only concession: on the operator's Mac the
batch is limited to that slice by `--slice <file>` so the rehearsal takes minutes, not hours; the
runbook for Sean's Mac has no slice.

Score sheet (filled in by the rehearsal, committed with its traces):

| # | result | evidence |
|---|---|---|
| A1 | | |
| A2 | | |
| A3 | | |
| A4 | | |
| A5 | | |
| A6 | | |
| A7 | | |
| A8 | | |
| A9 | | |

A rehearsal with any item failed does not go near Sean's Mac.
