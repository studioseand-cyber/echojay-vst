# Surgical EQ — Step B Spec: analyzer overlay + dynamic metering

For the local Claude Code session. Read with `SURGICAL_EQ_HANDOFF.md`. This is the last EQ feature: a live spectrum behind the curve and real-time metering of the dynamic bands' action. Editor + a small real-time-safe tap in the processor. Keep `EqEngine`/`EqMove` JUCE-free with their g++ tests green; commit/push to `feat/surgical-eq` in focused commits; rebuild + reinstall the AU (restart Logic to reload — version unchanged).

Everything here builds on what already exists: the editor's 30 Hz `juce::Timer`, the log-freq `freqToX`/`xToFreq` mapping, and `EqEngine::getBandDynamicGainDb(i)` (already implemented — returns the current dynamic gain contribution in dB for band i).

## Part 1 — pre-EQ audio tap in the processor (new, small, real-time-safe)

The editor needs recent input samples to FFT. Add a lock-free mono ring to `SurgicalEqProcessor`:

- Member: a fixed `std::array<float, 8192> analysisRing_` (or similar power-of-two) plus a `std::atomic<uint32_t> analysisWrite_` write index. No locks, no allocation.
- In `processBlock`, **before** `engine_.process(...)`, downmix the input block to mono (average channels) and write it into the ring, advancing `analysisWrite_` (release store). This taps the **pre-EQ input** — what's coming in — which is what you EQ against. (Post-EQ is a possible later toggle; do pre-EQ for v1.)
- Public read for the editor (message thread): `int readAnalysis(float* dest, int maxSamples) const` — copies the most recent `maxSamples` samples in order from the ring using an acquire load of the write index. A benign racy read is fine (it's a visualizer); no need to block the audio thread.

Real-time rules: the `processBlock` write must not lock, allocate, or call anything non-RT-safe. Just a memcpy-style copy into the ring with wraparound and an atomic index bump.

## Part 2 — spectrum analyzer in the editor

- Add a `juce::dsp::FFT fft_ { 11 }` (2048-point) and a Hann window (`juce::dsp::WindowingFunction<float>`). Reusable scratch buffers sized 2048 (real) / 4096 (complex), allocated once in the constructor — never in the timer/paint.
- On the 30 Hz timer (only when the analyzer is ON): `readAnalysis` the latest 2048 samples, apply the Hann window, run `fft_.performFrequencyOnlyForwardTransform`, convert each bin to dB (`20*log10(mag + tiny)`), and store into a persistent `std::vector<float> specDb_`.
- **Per-bin decay smoothing** so it's readable, not jittery: `specDb_[k] = max(newDb, specDb_[k] - fallRate)` (a fast rise, slow fall — e.g. fall ~ 12 dB per 100 ms). Tune to taste.
- **Draw it behind the curve** (before `paintCurves`/`paintNodes`, after the grid). Map to the same **log-freq x-axis**: for each x pixel, compute its frequency via `xToFreq`, find the FFT bin(s) at that frequency (bin = f * fftSize / sampleRate), and read/interpolate `specDb_`. This is the correct direction — the FFT is linear-freq, the axis is log, so sample the spectrum *per pixel* by frequency, don't map bins → x directly (low bins would bunch up).
- **Its own vertical scale**, independent of the EQ ±dB grid: map a spectrum-level range (e.g. floor −100 dBFS at the bottom of the graph to ceiling ~0 dBFS at the top) to y. Draw as a filled translucent path from the bottom of the graph up to the curve — subtle, so it sits *under* the cyan EQ curve without competing (a low-alpha fill, maybe a slightly different hue from the EQ curve so they're distinguishable).
- **Wire the "A" button** (currently inert, `setEnabled(false)` in Step A): enable it, make it a toggle that turns the analyzer draw + FFT work on/off. Default: on when hosted. When off, do no FFT work at all (cheap).

Performance: FFT runs on the message-thread timer at 30 Hz — trivial. Never FFT on the audio thread.

## Part 3 — dynamic-band metering

For each **enabled dynamic band** (`spec.dynamic == true`, Bell), the engine is applying a live gain change available via `proc_.getEngine().getBandDynamicGainDb(i)` (negative = gain reduction, e.g. de-ess pulling down). Visualize it so the de-ess/resonance action is visible:

- On the timer, poll `getBandDynamicGainDb(i)` for each dynamic band and store it.
- On the node: draw a **live gain-reduction indicator** at that band — the cleanest is a short vertical bar / fill from the band's static node position toward its *current effective* gain (staticGain + dynGainDb), so you literally see the band pumping down when it triggers. A small numeric "-4.2 dB" near the node is a nice optional addition. Use the band's dynamic colour (the amber already used for dynamic nodes).
- Keep it lightweight and smooth (the value is already envelope-smoothed by the engine, so no extra smoothing needed, but clamp/round for display).

## Acceptance
With the analyzer on, a live input spectrum sits softly behind the EQ curve on the same frequency grid, rising and decaying naturally. Toggling "A" turns it off with no residual CPU. A dynamic band visibly meters its gain reduction in real time when the input crosses its threshold. The g++ unit tests still pass; `EqEngine`/`EqMove` remain JUCE-free (all the new code is in `SurgicalEqProcessor` for the tap and `SurgicalEqEditor` for the drawing).

## Constraints (unchanged)
- Real-time safety in `processBlock` (the tap): no locks/alloc.
- No APVTS; editor talks to the processor via its typed accessors.
- Don't touch `ChainHost` or session-b files. This is self-contained to the EQ processor + editor.
- Commit/push to `feat/surgical-eq`; rebuild + reinstall the AU; restart Logic to reload.
