# Threshold-sweep traces, 29 Sep 2026 (batch 1)

Raw probe output from `ejmap --cert-sweep-all` over the 47 runnable products (kHs and townhouse
skipped), with the probe's `processes.json` per product and the driver's `run.jsonl`. These are
the MEASUREMENTS; fixtures are derived from them (`--cert-sweep-rederive`), so a rule change is
applied here without re-measuring (decision D2).

Not included: the batch's reports and fixtures. They were derived by a driver whose spec 4.7
"unlicensed" test was known-bad (six false withholdings) and which called a non-PACE window a
licence fact (fixed in dea571a).

Redaction: UAD's own logger prints the home directory ("Config file not found at ...",
"Adding log file at ..."). `/Users/<name>` was replaced by `~` in those lines only; no measurement
line was touched.
