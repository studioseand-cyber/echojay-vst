# C1 comp (m) ratio control, 19 Oct 2026 (Kathy's Mac, guard KEEP, second instance, niced, scratch folder)

- **The profile could not be used:** Sean's profile is 15.0.70 and this Mac has 12.0.0. `pass_lowref.log` shows the version gate refusing before any load.
- **What ran instead:** a labelled scratch control at 12.0.0 (`scratch_control_profile_12.0.0.json`, never exported).
  - Controls identical between the versions (8 controls, the same texts at 0 / 0.5 / 1).
  - The 15.0.70 profile's other-control values, written by norm and read back: Attack 2.00, Release 50, PDR Tc 0, Makeup 0.0, Output 0.0, Low Ref.
  - Positions at norm 0.8 and 0.6 (T about -25.6 / -45.8 dBFS), inputs T+3 .. T+42 in 3 dB steps, hold 2.5 s, read after settling.
- **Ratio dial placed from its own text** (65-point grid, landed reads only): 4.13:1 at 0.4844 (the profile's measured setting), 1.00 / 2.00 / 4.00 / 48.57:1. Each landed text was read back again in each call.
- **Second pass `--set 0:1.0` (Peak Ref):** the write landed ("Peak Ref", getValue 1.0). The output moved by a constant +28.45 dB at every level, and the GR is identical. The switch is a level reference, not a knee or detector setting. C1 has no knee control, so no hard-knee pass is possible.
- **Result:** the local ratio by GR depth crosses the label, overshoots it, then falls. 4.13 reads 1.49 @2 dB -> 4.17 @9 -> 5.77 @13 -> 2.75 @22 -> 1.86 @26 dB GR. The same holds at both positions (a static curve).
  - At no depth does the label hold within 0.5 dB.
  - The effective-ratio fit misses by 2.6-3.0 dB.
