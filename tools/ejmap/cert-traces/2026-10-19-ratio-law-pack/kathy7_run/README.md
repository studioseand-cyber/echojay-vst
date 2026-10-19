# Ratio law: Kathy's 7, run on Kathy's Mac, 19 Oct 2026 (pack app from cert 924b9ff3; nothing sent)

**What ran.** Plugins: Lindell 254E, Lindell SBC, Bettermaker Bus Compressor DSP, Solid Bus Comp, elysia alpha mix, bx_townhouse Buss Compressor, Unfiltered Audio Zip.
- The guard kept all 7, and the installed versions equal the served profiles'.
- Same flags as `ratio_law.sh`: the measured setting included, T-6..T+44 in 2 dB steps, g 2 / g 4 / deep positions.
- Every ratio setting was placed where the plugin printed a text, and every landing was read back (0 unread).

**Files.** One `<plugin_id>.ratio_law.json` per plugin (ej_ratio_law_run/2). Its `ratio_law` block carries `measured_ratios` and `curves` in the shape of RATIO_LAW_NOTE.md. Raw probe traces are in `raw/`.

**READ BEFORE USING THE CURVES: an offset at the measured setting.**
- At each plugin's own measured setting, this Mac's curve sits **2-4.4 dB to the right** of the served curve (`threshold_shift_db` at the measured ratio):
  - Zip +3.01 (hard knee: a clean shift);
  - Solid Bus +3.00;
  - townhouse +2.97;
  - SBC +2.43;
  - Bettermaker +2.16;
  - elysia +1.98;
  - 254E +4.36.
- Both sides use the peak level convention, and the plugin versions are the same.
- This run fed the sidechain bus with the same signal (all buses enabled); the served sweeps (3-5 Oct) do not record their bus policy. That is a lead, not an established cause.
- **Until it is explained, use these curves only relative to their own measured-setting curve,** not mixed with the served one.
- **The same check applies to Sean's run:** `threshold_shift_db` at the measured ratio should be about 0.
