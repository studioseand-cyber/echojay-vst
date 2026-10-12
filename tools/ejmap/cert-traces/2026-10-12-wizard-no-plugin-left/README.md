# No plugin left behind: rehearsal, 12 Oct (feat/ejmap-wizard)

The engine ran twice, as the app does: `run1_scan.txt` (scan), then `run2_measure_bundle.txt` (measure + bundle).

**Set-up**
- An empty wizard store; the stubbed lookup is `categories.json` + `pending_review.json` here.
- `--jobs 2`.
- The real `~/Library/ejmap`: 1123 maps before and after, and 0 files changed.

| plugin | why it is in the set | route | loaded? (`loads.log`) | result |
|---|---|---|---|---|
| Lindell SBC | the mock DB has it (rule A) | skip_have | **never** | ready: nothing sent |
| MBandPass | marked pending_review (another user's needs_category) | skip_pending | **never** | not re-sent; summary: "1 was already sent for review by someone else" |
| Massive (NI, `aumu`) | out of scope | out_of_scope by AU type | **never** | listed in `out_of_scope` (identity + why) |
| MChorusMB | server category "modulation" | send_other | map only | `other_categories`: map + "modulation" |
| elysia mpressor | server category "compressor" | measure | map, then measured | **measured**: profile + tone check in the bundle |
| MCombMB | from Sean's unsettled list: no category, no name word | categorise | map + signature reads (1 s) | `needs_category`: leaning reverb ("a tail, RT60 3.27 s, no word: leaning reverb (a feedback delay reads the same)"); the map (466 params) and the reads |
| Lindell 254E | a compressor, its catalogue entry removed, no name word | categorise | map + signature reads (2.6 s) | `needs_category`: leaning reverb ("a tail, RT60 0.40 s; reverb or delay: review"). See the finding below |
| MAutoVolume | no category, no name word | categorise → dynamics → compressor queue | map + reads + measuring | not measured: the compressor certification refused it ("not licensed suspected… intermittent dropouts", Melda demo mode). In `skipped` with that reason |

- **The summary** (run 2): "1 plugin ready, 1 newly measured, ready to send, 1 could not be measured, 2 plugins we couldn't identify were sent for review, 1 plugin of a kind we don't measure yet was sent so EchoJay knows it, 1 instrument / meter / utility was listed, not loaded, 1 was already sent for review by someone else". The scan run now says "will be sent" (pin WZ-ROUTE-4b).
- **The load ledger** (`loads.log`): every load in both runs is listed. Lindell SBC, MBandPass and Massive appear nowhere. Run 2 took the signature reads from `categorised.json` and loaded nothing again for them. A third scan re-run (cached) made 0 loads.

**Finding (not changed: the task says the same reads as EJ Map):** the categoriser checks for a tail before dynamics, so a compressor's release after a burst reads as "a tail". Lindell 254E, a known compressor, came out "reverb or delay: review". Sean's label fixes it per plugin. Fixing the rule is a categoriser change with its own equality, so it was not made here. The same instantiate-state reads can also differ between Macs: MMorph is eq on Sean's and dynamics here.

**Pins:** WZ-ROUTE-1..5 (plus 4b), 10 mutants all red. Suite 4086 checks, 0 failures. `categoriseOne` is the old `--categorise-propose` loop body, moved: the same decisions before and after on Lindell 254E, MAutoVolume, MCombMB and MMorph.
