# Beta database snapshot - 13 Oct (wizard 26badd11)

Snapshot: `tools/ejmap/wizard_app/beta_db`, built by `tools/ejmap/scripts/beta_db_snapshot.py "~/Downloads/cert 5"` (read-only).
659 files, 10.2 MiB; 678 plugin ids already mapped (256 profiles, 448 draft entries for 422 ids); 1218 categories by id + 1207 by name; 17 multibands (= the built-in list); 290 references. The personal scan found 0 hits.
Map fp is known for 109 of 256 profiles and 32 of 448 draft entries; the rest match by plugin id + version (as the lookup already did when the fp is unknown).

Plan on this Mac (`ejmap --wizard --plan`, an empty store, nothing loaded): 1376 AUs; 91 are out of scope by type/words with any database, leaving the 1285 the app scans.

| of the 1285 | count |
|---|---|
| skip (nothing loads) | 387 = 228 already mapped + 159 settled utility (72) / modulation (64) / meter (21) |
| measure | 618 = 473 queued + 145 rule B read-back first (85 EQ, 60 compressor; mostly UAD 11.2.0 vs cert 11.8.0) |
| sound-test (signature reads) | 272 |
| held (multiband, mapped only) | 8 (C4, C6, C6-SideChain, LinMB; the other 7 multibands are already mapped) |

Empty database, for comparison: sound-test 1210, measure 60, held 15.
`plan_rows.json`: every plugin's route with the snapshot and with an empty database.

The DMG at `~/Desktop/EchoJay-Wizard-Beta-2026-10-13/EchoJay Wizard Beta.dmg` has sha256 c4ff582d. It is signed 8BT5F9B887 (not notarised) and hdiutil verify passes. The bundle carries beta_db (370 entries), the icon is identical to WizardBeta.icns, and the engine strings carry 26badd11.
Rehearsal (`--only MCompressor --auto`, work + results in scratch): the app read beta_db from its bundle and returned MCompressor -> skip_have, with 0 plugins to map; screenshots are here.
