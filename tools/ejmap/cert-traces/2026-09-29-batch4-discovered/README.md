# Threshold-sweep traces, 29 Sep 2026 (batch 4: 118 discovered, the fresh-system configuration)

- EJ Map ledger: scan cache, `categories.json` (the prerequisite), and the map state fetched by
  EJ Map. There are no local maps.
- Fixture store: EMPTY. Every product came from discovery, not from a fixture list.
- 117 attempted (kHs Compressor skipped for its licence), PACE included, on mains with the lid
  open. The iLok was confirmed before the start.

Conditions:
- **The iLok dropped off USB at 21:43:08 and was re-seated at 22:07:06**, on AC with the lid
  open and no sleep. The only product that refused inside that window was MO-TT. Re-run with the
  iLok present, it still brought up PACE's window, so it is a genuine licence hold. The re-run
  wrote over its first raw file; both attempts are in `run.jsonl`.
- **API-2500 and H-Comp (m/s) certified FALSELY.** Their "reduction" is identical at -24, -12 and
  -6 (a gain law, not compression; the threshold appears coupled to make-up). No guard tested
  level dependence. Their fixtures are held back, the batch-2 H-Comp fixtures were removed from
  the store, and these traces allow a re-derivation once a guard is ruled.

Redaction: `/Users/<name>` was replaced by `~` in plugin logger lines only.
