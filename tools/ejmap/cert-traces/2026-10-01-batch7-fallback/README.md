# 2026-10-01 batch 7: the quiet-level fallback on the 22 contaminated-reference products

The 22 fixtures (16 product names; (m)/(s) pairs separate) whose soft-end reference failed the
relative guard on 30 Sep, re-swept from scratch against a store copy without their fixtures so the
fallback could run: full sweep, then a re-sweep of the refused candidate with the per-position
quiet reference (-54/-48). Two sessions: 7 products on the evening of 30 Sep (18:56-19:42; power
dropped at 19:33 and the Mac slept from 19:56 - C6-SideChain (s), caught mid-product with 1,708 s
of sleep, was discarded and re-run), then 15 from 13:44 on 1 Oct once mains returned. iLok on
0x00130000 / 1 throughout the second session (0x01120000 / 2 in the first, dropped at 19:00).
`pile.log` has the per-product port and power.

THREE OUTCOMES, as agreed (quiet check = -48 minus -54 within 0.1 dB of -6, per position):

- RECOVERED via the fallback (24 candidates, 18 products): Solid Dynamics, Drawmer 1973 Low,
  C6 (m/s), C6-SideChain (m/s), C4 (m/s) [6 of 16 positions pass the quiet check: at the hard end the
  band threshold sits below -48 dBFS], Renaissance Axx (m/s) [5 of 16], MTurboCompMB Band 2 [16/16],
  MTurboComp [15/16], MDynamicsMB (4 candidates, 14-15/16), MDynamicsMBLarge (3 of 4),
  **SPL IRON L and R [41/41 - every step of a stepped control]**, Lindell 254E, Lindell 354E Mid,
  **SSL Native Bus Compressor 2 [8/16]**.
- QUIET CHECK FAILED like OTT: none.
- STILL REFUSED: LinMB (m/s) Band 3 - nonmonotonic on the 7 of 16 positions the quiet check
  allows; MDynamicsMBLarge Band 3 Processor 1 - nonmonotonic (its three siblings recovered).
- NOT RUN: SSLGChannel (m/s) - "not on the worklist": they have no map in the ledger, so once
  their fixture is removed discovery cannot offer them. Queued separately with their record
  stripped from the store copy (run_ssl.sh, after the tuner batch).

THE DISCRIMINATING PREDICTION: the multibands should recover (other bands compressing at default
is what a -48 dBFS tone sits below), SPL IRON and SSL Native Bus Compressor 2 - not multiband -
might not. BOTH RECOVERED. So the mechanism is broader than "other stages": a single-band
compressor whose SOFT END still compresses at -24..-6 dBFS (its threshold range does not reach
a linear setting at the test levels) contaminates its own reference the same way, and the quiet
reference repairs it the same way. The 8-19% band was two causes with one cure.
