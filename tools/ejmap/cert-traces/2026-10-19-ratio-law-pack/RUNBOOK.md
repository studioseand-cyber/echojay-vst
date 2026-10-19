# Ratio law spot check: runbook

**Why.** Your server dials a certified compressor at its measured setting from our measured curve. At any other ratio setting it needs the true (effective) ratio of that setting. This check measures each setting's effective ratio and says whether GR = (input - T) x (1 - 1/ratio) holds there within 0.5 dB, both with the label and with the effective ratio, from the knee upward, inside the tested range only.

**Two lists** (B's 27 adjustable single-ratio profiles, 18 Oct; the 18 fixed-ratio are out of scope):
- `products_sean_mac.txt`: 20: the 7 licence-bound on Kathy's Mac (MCompressor, CL 1B, VBC FG-Red: PACE; the three SSL Native; kHs Compressor) and the 13 Waves profiles (API-2500 m/s, DPR-402, the six C1 variants, SSLComp m/s, RCompressor m/s), served at 15.0.70 while Kathy's Mac has 12.0.0. **Run these on your Mac.**
- `products_kathy_mac.txt`: 7 whose installed version equals the served profile's (Lindell 254E, Lindell SBC, Bettermaker, Solid Bus Comp, elysia alpha mix, bx_townhouse, Zip). Kathy runs these.
- A profile is never used across versions: the run refuses a plugin whose installed version differs from the profile's, before loading it.

## 1. Unzip and clear the download flag (once)
```
cd ~/Downloads/RATIO_LAW_PACK
xattr -dr com.apple.quarantine .
```

## 2. The plan (loads nothing)
```
bash ratio_law.sh --dry-run products_sean_mac.txt
```
For each product it lists the probe calls:
- each ratio setting (the lowest, 2:1, 4:1, the highest; the measured setting is skipped) at the tone check's g 2 and g 4 positions;
- 3 input levels above threshold each.

A ratio control whose readings are non-linear or named (C1's, DPR-402's) says "placed at run time": one extra load reads the plugin's own ratio text on a grid, with no audio, after the licence gate.

It also prints the law at the measured setting from your existing sweep. A unit whose slope changes with depth misses there at its label (the CL 1B by up to 5.2 dB). The note says why one ratio per setting does not describe such units.

## 3. The run
```
bash ratio_law.sh products_sean_mac.txt
```
- The preflight runs first (mains power, the probe's signature). If it refuses, nothing runs.
- Licence gate first. A licence window stops that product: closed, never clicked.
- A few minutes per product. Results go to `~/Library/ejmap/cert/ratio_law/`, one `.ratio_law.json` per product with the raw traces in `raw/`.

## 4. Send the results to Kathy
```
cd ~/Library/ejmap/cert && zip -rq ~/Desktop/ratio_law_19oct.zip ratio_law
```
She scores them again on her side (`ejmap --ratio-law-score <file>`) and proposes each profile's `ratio_law` block with its measured-ratios table.
