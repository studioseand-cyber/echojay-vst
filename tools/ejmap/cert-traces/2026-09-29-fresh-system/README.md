# The fresh-system test, 29 Sep 2026: elysia mpressor, end to end, no stub

- EJ Map ledger: a fresh directory holding only `scan-cache.xml` (standing in for the first-run
  scan) and `categories.json` (the documented server prerequisite).
- Map state: fetched by EJ Map itself (`--sweep --dry-run`, 72 batches, 1,799 identities).
  It has no local maps.
- Fixture store: an EMPTY directory.

`--cert-sweep-census` discovered 158 compressors from the ledger: 118 runnable with PACE
included, 91 with it held. elysia mpressor was among them: "mapped (server map state 3)". It has
no local map; another machine mapped it.

`--cert-sweep --product "elysia mpressor"` then ran with no human in the loop:
- defaults sampled at the installed 1.15.1 (18 controls, readouts checked by instantiating twice)
- the name rule picked [3] Threshold (dB) and [9] ratio ("2.8")
- 20 processes, 0 retries, 16 writes landed in-stack
- certified, higher_is_harder: engage drift 0.04, offset IQR 0.10, displayOffsetDb -19.85

The fixture is `cert-fixtures/compressor-profiles/AudioUnit_49696d78_1.15.1.json`. These traces
re-derive it.
