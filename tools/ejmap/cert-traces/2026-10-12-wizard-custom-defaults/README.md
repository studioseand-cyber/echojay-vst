# Wizard: known multibands and custom defaults, rehearsal 12 Oct (feat/ejmap-wizard)

**wref1: the truth today.** The references for MMorph and MTurboComp come from Sean's traces only (`agreeing: 1`).
- **OTT:** "held: no server category (a known multiband: EJ Map's list)". Only mapped (its parameter list goes with the held entry); **no compressor pass**. Before this it spent 591 s in the compressor certification and was refused.
- **MMorph:** `custom_default`, "20 of 112 control(s) open at another value than the reference; the reference is not trusted (1 source(s), 2 needed)". Not measured.
- **MTurboComp:** `custom_default`, 5 of 73 controls, the same reason. Not measured.
- **Summary:** "2 plugins open with a custom default and were not measured".
- `loads.log` shows the opening-block reads ("Measuring MMorph / MTurboComp") since 67f4149b. Before that, the reference check's loads had no marker or ledger line.

**wref2: the restore path.** The same references are marked as agreed by two Macs. This is a **stated rehearsal assumption**, to exercise the write-back.
- **MMorph:** 20 writes back to the reference (e.g. Compression (A) 25.0% → 16.7%, Tone (A) −6.00 → −3.61 dB). The read-back matched, so it was measured. Then its certification stopped on Melda's demo dropouts: `needs_licence`, 951 s.
- **MTurboComp:** 5 writes (Ratio 50% → 0%, Attack 100% → 25%, Device → "Meldei 1176"). The read-back matched, so it was measured. **All 38 of its measuring traces start with the five writes in force and landed** (`preset` lines). The certification then refused it: "not licensed suspected… intermittent dropouts" (the same demo mode).
- So the restore mechanism works end to end. Neither plugin can complete a profile on this Mac, because these paid Melda plugins are unlicensed here.

Rows: `wref2_row_*.phaseb.json` (`reference`, `custom_default_restored` with control / opened / written / norm).
