# 2026-09-30 batch 5b: the candidate pile, items 27-42

The remaining 16 of the 42-product pile (`pile.txt` order), run 13:27-13:50 on 30 Sep with
`--cert-sweep` per product against a copy of the repo store (so the 26 from batch 5 were
recorded, not re-run), ledger `fresh4`, `--include-pace`, mains, lid open. Driver at 575857f
(refusal records, unclean budget 2, dispositions honoured). MDynamicsMB was re-run from scratch;
its batch-5 partial is superseded.

- 15 products swept to a candidates fixture (in `cert-fixtures/compressor-profiles/`).
- 1 refused: SSL G3 MultiBusComp, stage `defaults`, TRANSIENT - PACE's activation window at
  `--list-params`. Cause: **the iLok dropped off USB during MTurboCompMB (between 13:37:03 and
  13:40:57; port 0x01130000 / 2 before, none after, still absent at 13:52)**. Every product from
  OTT on ran without the dongle; SSL G3 is the only PACE-bound one among them and the only
  casualty. Its refusal record is in the store; re-seat the iLok and run with `--retry-refused`.
- `pile.log` is the runner's log with the per-product iLok port and power source (the field that
  was empty in batch 5). `defaults/` holds the defaults sidecars (the samples each sweep was
  planned on), kept out of the store as in batch 5.

Not diagnosed, recorded: MDynamicsMB (not Large) shows single-position spikes (34.4 dB at
position 2 and 7 of Band 2 Processor 1; 4.9-5.1 dB isolated positions on Band 2/3) that make
three candidates nonmonotonic, while MDynamicsMBLarge's same bands are clean. And the
licence-suspect rule (silent at default) fired on 9 Melda candidates that are GATE or
Processor-2 stages - a gate closing on the tone is silent, not unlicensed.
