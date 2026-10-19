# The Zip +3.01 dB offset, 19 Oct 2026 (Kathy's Mac, guard KEEP, scratch, niced)

**Cause: the ratio law's own convention bug, not the layout.**
- Served profiles give every dBFS as sine RMS (EjmapProfileExport: our peak - 3.0103).
- The ratio law took T from the served curve and compared it with PEAK inputs, and wrote its curves in peak.
- Fixed in EjmapRatioLaw.h (pin WZ-RATIO-7): served values are converted to peak on the way in, and the curves go out in sine RMS with `level_ref`.
- Re-scored, Zip verifies 1 / 1.94 / 4.14 / 100:1 with a 0.00 dB shift. Its 4.14 curve matches the served curve to 0.04 dB at norm 0.4.

**Layout: no effect.**
- `zip_*.txt`: Sean's served pos20 call replayed under silent / unconnected / echojay / self matches his readings to 0.000 dB at 36 levels.
- `replay7/`: one served position per plugin, for all 7 of Kathy's, under unconnected (a DAW insert with no sidechain source), echojay (the pack) and silent. Worst difference from Sean's readings: 0.016 dB (townhouse); every other plugin 0.000.
- `served_policy_27.txt`: 20 served profiles were swept with the sidechain fed silence, 7 unconnected.
- Every one with a sidechain input carries cert 6's own A/B against the current self-keyed policy: "same".
- Mono was not tested: the probe has no switch for it, and the served sweeps and the pack both run the plugin's declared stereo layout.
