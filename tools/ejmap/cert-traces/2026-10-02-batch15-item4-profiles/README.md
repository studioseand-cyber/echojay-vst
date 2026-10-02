# 2026-10-02 batch 15: nine licence-free compressors as v1.4 profiles (10:51-11:08, mains; the iLok LEFT at 10:54:13 - ilok_watch.log)

Run by the item-4 runner (run_item4.sh, one --cert-sweep --profile per product, no --include-pace) with the
after-chain by hand (after_item4.log); then the SAME records finished by the new loop's finish pass
(loop_smoke.log, 11:17: 9 rows, outcomes.json, census.txt) - the first live run of ONE LOOP, with the
254E row corrected once the loop learned a failed tone check is a result. Records copied to the store.

| product | positions (added) | rungs below -54/-48 | detector_f | in_at_gr[2] numeric | point_error_db | tone check |
|---|---|---|---|---|---|---|
| Acme Opticom XLA-3 1.10.1 | 24 (8) | 3 | 0.73 | 22 of 24 | 0.0 | 2.0 PASS |
| Bettermaker Bus Compressor DSP 1.0.0 | 16 (0) | 0 | 0.93 | 12 of 16 | 0.1 | 1.98 PASS |
| Lindell 254E 1.2.2 | 16 (0) | 0 | 0.83 | 16 of 16 | 0.1 | None FAIL |
| Lindell 7X-500 1.2.2 | 22 (6) | 0 | 0.13 | 21 of 22 | 0.0 | 1.96 PASS |
| Lindell SBC 1.0.3 | 16 (0) | 0 | 0.0 | 16 of 16 | 0.1 | 1.99 PASS |
| bx_opto 1.10.1 | 27 (11) | 4 | 0.63 | 23 of 27 | 0.2 | 1.99 PASS |
| bx_townhouse Buss Compressor 1.8.1 | 16 (0) | 0 | 0.7 | 14 of 16 | 0.1 | 1.96 PASS |
| elysia alpha mix 1.17.1 | 16 (0) | 0 | 0.23 | 11 of 16 | 0.1 | 2.0 PASS |
| elysia mpressor 1.15.1 | 20 (4) | 0 | 0.4 | 20 of 20 | 0.0 | 1.97 PASS |

Lindell 254E: the tone check's quiet reference FAILED at the picked position (GR unreadable) - a result on the
profile, logged as a lead, not chased. Everything else passed within 0.04 dB of g = 2.
