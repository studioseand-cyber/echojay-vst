# `a/bandpeak-256` is PARKED, not pending (28 Oct 2026, 17:05)

**Do not merge this branch.** Sean's ruling, after K's confirmation test:

> *"Kathy's drums vs music bed test: V1 crest doesn't separate a real drum excess from a body excess
> (a 0.13 dB gap), so crest is directional only before the beta and never decides. Keep the
> 256-sample bandPeak branch unmerged and parked. Note its CPU figures in your report for after the
> beta."*

So the thing this branch was built for — a crest that could **decide** — is not happening before the
beta. What the branch itself does is unaffected and still correct:

- `bandPeak`'s band FFT runs on a **sample-counted 256-sample cadence** instead of once per
  `processBlock`, so the figure stops moving with the host's block size (her MA10 drift was up to
  0.38 dB from block size alone, and this holds it under **0.003 dB** across 128/512/1024).
- **`bandPower` is untouched**, bit-for-bit, and the window stays 2048 (no second FFT size).
- **Off by default** (`setFinePeakCadence`), so no live meter pays for it.
- `HopFrame::peakFrames` records how many frames the peak was over, appended at the end of the
  struct.

## What is still worth having from it, after the beta

1. **The cadence itself**, if any figure derived from `bandPeak` is ever asked to decide anything. A
   max over a frame count the host chooses is not a measurement, whatever it is later used for.
2. **The cost figures**, which is what Sean asked to keep: `tools/bandpeak_cadence_guard` prints
   microseconds of CPU per second of audio, per engine, at 128/512/1024 blocks, cadence off against
   on. They are in the day's report (`~/Desktop/FOR_REVIEW_22oct/DAY_A_28oct.md`).
3. **The guard's own control**, which is the part that makes the 0.003 dB bound mean anything: the
   same signals with the cadence off must still drift.

## What it must not be read as saying

`BP3` separates a synthetic click train from a synthetic tone. That is the easiest case there is and
it is **not** her stems: her drums-against-body gap is **0.13 dB**, which is why the crest is
directional only. The guard says so where it is asserted, too.

Branch cut from `871acf59`; the work is `07fe5e17`. Worktree `~/echojay-vst-bp256`, which builds and
self-tests there and installs nothing.
