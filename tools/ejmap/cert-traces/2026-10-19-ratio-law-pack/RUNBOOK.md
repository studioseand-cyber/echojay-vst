# Ratio law spot check: runbook (Sean's Mac)

**Why.** Your server works out a certified compressor's gain reduction as GR = (input - threshold) x (1 - 1/ratio). For a profile measured at one ratio only, that maths is safe at another ratio only if the plugin's ratio control behaves:
- the true ratio is the labelled one;
- threshold and knee don't move with ratio.

This checks that, on your Mac, which holds the licences (the CL 1B is licence-bound).

## 1. Unzip and clear the download flag (once)
```
cd ~/Downloads/RATIO_LAW_PACK
xattr -dr com.apple.quarantine .
```

## 2. The plan (loads nothing)
```
bash ratio_law.sh --dry-run
```
For the CL 1B it should list 6 probe calls:
- 2:1, 4:1 and 10:1 (6:1 is the measured ratio, so it is skipped);
- at the tone check's g 2 and g 4 positions;
- 3 input levels above threshold each.

It also prints the law at the measured 6:1 from your existing sweep. For the CL 1B that **already misses by up to 5.2 dB**: your sweep's slope there is 0.45-0.54, while a true 6:1 would be 0.83.

## 3. The run
```
bash ratio_law.sh
```
- The preflight runs first (mains power, the probe's signature). If it refuses, nothing runs.
- Licence gate first. A licence window stops that product: closed, never clicked.
- A few minutes per product. Results go to `~/Library/ejmap/cert/ratio_law/`, one `.ratio_law.json` per product with the raw traces in `raw/`.

## 4. Send the results to Kathy
```
cd ~/Library/ejmap/cert && zip -rq ~/Desktop/ratio_law_19oct.zip ratio_law
```
She scores them again on her side (`ejmap --ratio-law-score <file>`) and proposes each profile's `ratio_law` block.

## Adding products
Add names to `products.txt`, one per line, exactly as EJ Map names them. Only certified compressors measured at one ratio: B's list. Products with no ratio control are skipped (their maths stays at the fixed ratio).
