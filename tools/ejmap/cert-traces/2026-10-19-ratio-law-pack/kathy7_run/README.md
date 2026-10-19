# Ratio law: Kathy's 7, run on Kathy's Mac, 19 Oct 2026 (scored by the pack at cert 6802eefa; nothing sent)

**What ran.** Lindell 254E, Lindell SBC, Bettermaker Bus Compressor DSP, Solid Bus Comp, elysia alpha mix, bx_townhouse Buss Compressor, Unfiltered Audio Zip.
- The guard kept all 7, and the installed versions equal the served profiles'.
- Every ratio setting was written where the plugin printed a text, and every landing was read back (0 unread).
- The measured setting is included; inputs T-6..T+44 in 2 dB steps; g 2 / g 4 / deep positions.

**Files.** One `<plugin_id>.ratio_law.json` per plugin. Its `ratio_law` block carries `measured_ratios` and `curves`, **in sine RMS dBFS like the served profile** (`level_ref`). `summary.txt` has one line per plugin; the raw traces are in `raw/`.

**The earlier "2-4.4 dB offset" is resolved.** It was our scorer reading the served curve (sine RMS) as peak.
- Re-scored, the shift at each measured setting is 0.0-0.9 dB, the remainder being soft-knee curvature in the line fit.
- Zip (hard knee) shows 0.00 and verifies at every setting.
- Layout was tested and has no effect (cert-traces/2026-10-19-layout-and-convention).
