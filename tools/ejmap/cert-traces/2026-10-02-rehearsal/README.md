# 2026-10-02 the stranger's-Mac rehearsal — three runs, one verdict (docs/STRANGER_MAC_TEST.md)

Paths redacted (`~`); no `config.json` (the sign-in token) anywhere here. The scripts that drove each
run are at the top level (`run.sh`, `unbroken.sh`, `mapping.sh`, `unbroken2.sh`); `slice2.txt` is the
slice; `census_existing*.txt` are the existing ledger's census under the old and the new binary, for
the reconciliation by name.

- `batch-rehearsal/` (11:59–12:49, seeded ledger from the 4 Aug scan + 26 Aug categories, old rules):
  NOT the test — the batch exercised alone: interrupt during product 3 and resume (A8), the states,
  EMO-D5 (s) 2583 s. Lost elysia mpressor and bx_crispytuner silently (no map in the seed) — the
  finding that made `unmapped` a named state, later a field.
- `run1/` (13:27–14:27, EMPTY ledger, before the ruling): the first unbroken run. Found the scan's
  licence windows (94 stops once the window watch existed), the categorise hand-off (the endpoint
  asks only about the mapping worklist and sends no mark_keys → 0 categorised identities → nothing
  to do), and the disposition gate (202 held, every tuner). `mapping_step_run1.log`: Lindell 7X-500
  mapped locally in 6 s, certified from the local map after its server entry was removed; the mapping
  sweep declined bx_crispytuner as no_dial_set even when targeted.
- `run2/` (14:43–15:43, EMPTY ledger, the ruling applied): **THE TEST.** `unbroken2.log` is the whole
  run; `cert/` is exactly what the zip handed over (outcomes.json 104 rows, census.txt, census-step3,
  batch.log, fixtures, profiles with tone_check embedded, raw traces, mapping.log,
  census-after-mapping); `ledger/` the scan's artefacts (licence-stops.json 94, categories.json with
  the one labelled stand-in, map-state.json with Lindell's entries removed and `test_edit` saying so,
  the one local map). Scores A1–A8 pass, A9 blocked on the catalogue.

Timings (run 2, M1 Pro 16 GB, mains, iLok absent): scan + categorise 362 s (94 licence stops, median
2.7 s each, 319 s of the total; 1733 rows; 1073 catalogue answers); census 7 s; bx_crispytuner (tuner)
12.9 s; bx_opto 59.8 s; elysia mpressor 105.7 s; EMO-D5 (s) 2589 s (channel strip, 5 candidates);
Lindell 7X-500 123 s (no map); Lindell SBC 126.6 s; NEOLD U2A 166 s (engage search); NEOLD V76U73
13.7 s (refused at plan); CL 1B 0 s (carried forward); APB 0 s (held); batch 3228 s; mapping one product
5 s; zip 1 s.
