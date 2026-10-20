# Ratio law results: Sean's 20 (re-scored) + Kathy's 7, 20 Oct 2026

Nothing was loaded or sent to produce this. `sean_mac/` is Sean's 19 Oct run (the earlier pack) **re-scored** on our side; `kathy_mac/` is Kathy's 7 run with the fixed pack. Every file is in the final shape: one `<plugin_id>.ratio_law.json`, its `ratio_law` block carrying `status`, `safe_ratios`, `measured_ratios` and `curves`, **in sine RMS dBFS like the served profiles** (`level_ref: sine_rms_dbfs`).

## What the re-score did (Sean's files)
- **Each setting is labelled by what the plugin printed back**, never by the ratio aimed at. A landing with no read-back is left out (none remained).
- **The +3 dB "MOVED" on every setting was our bug,** reading your sine-RMS curve as peak.
  - Re-scored, every plugin whose measured setting was read back reproduces your served curve within **0.13 dB on all 12 keys** (`curve_vs_served.txt`): C1 ×6, API-2500 ×2, CL 1B, SSLComp ×2, VBC FG-Red, DPR-402.
  - The `threshold_shift_db` figures that remain (CL 1B +1.69, C1 +0.62) are a straight-line fit across a soft knee, not an offset. The curves themselves match.

## What B may load
- **Load `ratio_law.curves`** for every plugin here except SSL Native Bus Compressor 2 (no data).
  - Each entry is one ratio setting (`ratio`, the plugin's own `set` text, the `norm` written) with its curve per threshold position, in the `amount.curve` shape.
  - Dial a setting from its own curve.
- **Use the formula GR = (input - T) x (1 - 1/ratio) only where `measured_ratios[].use_effective` is true**, at that setting's `effective` ratio. Elsewhere, use the curves.
- **Don't** extrapolate past the lowest / highest `tested_ratios`.

## Per plugin, in plain English
**Verified: the formula holds at every tested setting.**
- SSL Native X-Comp v6: 1, 1.04, 3.93 and 50:1.
- Unfiltered Audio Zip (Kathy's Mac): 1, 1.94, 4.14 and 100:1.

**Partial: the formula holds at some settings, curves for the rest.**
- kHs Compressor: safe at 3.1:1 and the limit; not at 1.5:1.
- SSL Native X-ValveComp v6: safe at 1.44, 3.26 and 4.97:1; not at 30:1.

**Curves only: use the measured curves; the formula does not hold.**
- C1 comp, comp-gate and comp-sc (m and s; "safe" only at 1:1, which compresses nothing).
- API-2500 m/s, Tube-Tech CL 1B, SSLComp m/s, DPR-402 (m), RCompressor m/s.
- MCompressor and VBC FG-Red (1:1 only).
- Kathy's Mac: Lindell 254E, Lindell SBC, Bettermaker Bus Compressor DSP, Solid Bus Comp, elysia alpha mix, bx_townhouse Buss Compressor.

**No data:** SSL Native Bus Compressor 2 stopped on a PACE window. It is in the top-up.

## Coverage: 26 of the 27 have usable per-setting curves now
- **Complete:** 19 (Sean's 12 + Kathy's 7).
- **Some settings missing until the top-up:** kHs Compressor (its measured 2:1, 2:1, 4:1), RCompressor m/s (measured 4.34:1, 4:1), MCompressor (measured 1.8:1, 2:1, 4:1), X-ValveComp (2:1, 4:1), X-Comp (2:1), VBC FG-Red (2:1).
- **None:** SSL Native Bus Compressor 2.

## Top-up for Sean: `products_topup.txt`, 8 plugins, about half an hour
Run once with the fixed pack: `bash ratio_law.sh products_topup.txt`. **Open SSL Native Bus Compressor 2 once in Logic first** so its licence is authorised. Send the results folder as before.
