# 2026-10-01 batch 9: the first tuner measurements (15:39-15:42, mains, iLok 0x00130000 / 1)

`--cert-tuner` on eight products: defaults pass, tuner-lexicon roles, then every strength-role
control swept with a static +30 cent A3 (strength) and a square ±30 cent vibrato at 0.5 Hz
(speed), 8 positions, one process per generator. Records in `cert-fixtures/tuner-profiles/`.

| product | roled control | strength | speed | notes |
|---|---|---|---|---|
| Auto-Tune Pro 11.5.1 | Retune Speed [4] (0..400) | 1.0 at 8/8 (0.994 at '400') | 565, 416, 256, 139, 85, 48 ms at '226'..'6'; '0' = bound < 21 ms; '400' refused: not settled within the 1 s half period | key/scale 'Chromatic' |
| Auto-Tune EFX+ 10.5.0 | Retune Speed [12] | 1.0 at 8/8 | 565, 421, 256, 149, 85, 53 ms; '0' bound; '400' refused | key 'C' |
| Auto-Tune Artist 9.5.0 | Retune Speed [6] | 1.0 at 8/8 | REFUSED at every position but '0' (bound): "not settled before the next flip" even at '6' | LEAD: same display values as Pro/EFX+, different outcome - 9.5 vs 10.5/11.5, or a trace shape the guards read differently; not chased |
| Auto-Tune Access 10.5.0 | Retune Speed [1] Slow/Medium/Fast | 1.0 at norm 0 and 1 only | refused (not settled) at 0 and 1 | 6 of 8 writes DID NOT LAND: a 3-text stepped control reported as continuous; the norms list needs the text-step scan (`--sample-text`) - LEAD, small fix |
| bx_crispytuner 1.1.0 | Amount [12] (0..100) | 0.0, 0.27, 0.27, 0.54, 0.54, 0.80, 1.07, 1.07 - a strength curve in steps; residual -2.1 cents at 86/100 = over-correction past the note | 133-165 ms at 0..71, 53 ms at 86/100 | key/scale 'None' |
| Auto-Tune EFX 9.0.1 | - | - | - | defaults --list-params exit 3 (probe refused); not diagnosed |
| MetaTune 1.1.8 | - | - | - | PACE activation window at --list-params (iLok present, product not licensed here): transient |
| Melodyne | - | - | - | refused by name before any process: ARA/offline-only, uncertifiable by any harness |

What the harness showed on its first live run: the speed derivation reads a clean monotonic
duration curve off two products (Pro, EFX+) and refuses honestly where the half period is too
short for the slowest setting; the static strength is 1.0 everywhere on Antares (full correction
of +30 cents) and a real curve on bx_crispytuner. No number was reported without its guards passing.
