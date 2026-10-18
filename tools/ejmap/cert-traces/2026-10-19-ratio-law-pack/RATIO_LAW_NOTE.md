# The ratio law: a note for Sean (and B)

**The question.** A certified profile measured at one ratio: may the server use GR = (input - T) x (1 - 1/R) at another ratio R?

**The answer we propose:** only where a spot check has shown the ratio control behaves, and only between ratios that were actually tested.

## The check
- **Where:** `RATIO_LAW_PACK`, run on your Mac (licence-bound plugins).
- **Settings:** the ratio control at the lowest, 2:1, 4:1 and the highest ratio (the measured ratio skipped).
- **Positions:** the tone check's g 2 and g 4 picks.
- **Input:** a steady tone at T + 6, 9 and 12 dB.
- **Scoring:**
  - T comes from your existing sweep at that position: the line through its 2 dB and higher points.
  - Predicted = (input - T) x (1 - 1/R_label), against the measured GR.
  - A point passes within **0.5 dB**.
  - Points less than half a knee above T are **left out, never failed**. The knee is the profile's `ratio.knee_db`, else 6 dB.
- **Reported per ratio:** the effective ratio from the slope, and how far the threshold moved (more than 0.5 dB = "moved").
- **Verdict:**
  - "ratio law verified" when every tested ratio passes;
  - otherwise the list of safe ratios;
  - **never** anything past the lowest or highest tested ratio.

## The CL 1B: read this before using the maths at its own 6:1
The profile already says what the check is likely to find:
- `ratio.curve[0]` is "6:1" with `measured_ratio` 1.74;
- `fit.model` uses ratio 1.5.

Your existing sweep, scored against the law at 6:1 with no new load, misses by **up to 5.2 dB**. The gain reduction rises at 0.45-0.54 dB per dB, where a true 6:1 would be 0.83. The CL 1B is an opto: its "Ratio" knob does not set a hard ratio.

So until the check runs, **the maths at 6:1 itself would over-predict GR on the CL 1B**. Using the profile's own measured curve (in_at_gr), not the formula, is what passed the tone check.

## The profile field (proposed): `ratio_law`
```json
"ratio_law": {
  "status": "verified" | "partial" | "none",
  "tested_ratios": [2, 4, 10],
  "safe_ratios": [2, 4],
  "safe_between": [2, 4],
  "worst_db": 0.31,
  "knee_db": 6.0,
  "measured_ratio": 6.0,
  "effective_ratio": {"2.00": 2.02, "4.00": 3.96, "10.00": 6.1},
  "threshold_shift_db": {"2.00": 0.1, "4.00": 0.0, "10.00": 1.4},
  "rule": "GR = (input - T) x (1 - 1/ratio), T from the measured sweep; pass within 0.5 dB at every point from the knee upward; never past the lowest or highest ratio tested"
}
```
The numbers above are an example, not the CL 1B's.

- **verified:** the maths at any ratio between the lowest and highest tested.
- **partial:** the maths only at the `safe_ratios`, and between two of them only inside `safe_between`.
- **none:** the maths at no ratio but the measured one, if that one passes.
- **A profile without the block: the maths only at its measured ratio.** That is the default, and it is today's behaviour.

## What your server must do first
**`lib/compProfile.js` must accept `ratio_law` before we send a profile that carries it.** Today its validator may reject an unknown field, as it did `neutral[].control ""`.

Until then we send no profile with the block. The default above holds.

Nothing has been sent.
