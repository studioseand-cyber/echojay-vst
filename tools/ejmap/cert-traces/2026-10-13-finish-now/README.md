# Finish now, the time line, limiters - 13 Oct (wizard 714ac57a)

DMG: `~/Desktop/EchoJay-Wizard-Beta-2026-10-13/EchoJay Wizard Beta.dmg` (sha256 59b9d875). It is signed 8BT5F9B887, not notarised, and hdiutil verify passes. The bundle carries beta_db with 52 limitercomp files, the icon is identical to WizardBeta.icns, and the engine strings carry 714ac57a. Both READMEs (repo + Desktop) are updated: "Finish now" replaces "closing stops it".

## Rehearsals (beta app, scratch work/results, Waves v12: W43, OneKnob Pumper, DeEsser, C1 gate; --jobs 1, Finish at 1 measured)
1. Finish fired 15 s in on "1 measured so far". The 1 was OneKnob Pumper: its row landed ok, but the certification refused it as a ducker. The in-flight W43 was discarded with no row and no temp folder; 2 plugins were not_reached; the bundle had 0 profiles. **Defect:** the count was landings, not results. Fixed: `wizroute::landedMeasured`, WZ-FIN-2. Screenshot: `rehearsal1_miscount.png`.
2. Finish fired as DeEsser landed (21:18:01). The gate pass then started and ran its full 4.5 min. **Defect, older than Finish now:** a stop between two passes was read only after the next whole pass, and it delayed a pause the same way. Fixed by checking before each pass and in Phase B's stop test (WZ-FIN-3).
3. On the fixed build, Finish fired as DeEsser landed (21:38:02) and the engine ended 1.3 s later. The gate was in flight, discarded and filed not_reached. DeEsser measured; OneKnob Pumper and W43 could not be measured. The results zip holds DeEsser's draft only.
4. On the signed app: DeEsser + C1 gate, Finish at 1. Screenshots: `measuring_finish_now.png`, `measuring_finishing.png`.

Not rehearsed: the close-window question (finish / pause / keep measuring). `--auto` skips it, and this process cannot capture other windows.

## Time line
`scan_done_time_line.png` comes from a STAGED work folder: scanned state, plus `to_measure` 625 and `to_measure_eta_s` 98513 taken from `ejmap --wizard --plan --jobs 2` on this Mac. That is the real per-category estimate: 649 measurements, starting figures for every kind, a quick check counted as a measure, about 27 hours. A real full scan was not run because it would load the licence-bound vendors.

## Limiters and this Mac's numbers
13 limiters in cert 5 have only the limiter draft and 26 have both passes. 6 of the 13 are installed here at the same version; each now gets its compressor pass: APB L-18 Limiter, MLimiterX, MLimiterMB, MUltraMaximizer, bx_clipper, bx_limiter True Peak.
Of the 1285 scanned: skip 381 (222 already mapped + 159 settled utility/modulation/meter), measure 624 (479 + 145 rule B read-back first), sound-test 272, held 8.
Of all 1376 AUs: skip 471, measure 625, sound-test 272, held 8. The 91 that are "out of scope by name" with an empty database include 3 the database knows: bx_digital V2 Mono is a read-back check, and bx_dynEQ V2 Mono and kHs Phase Distortion are already mapped.
