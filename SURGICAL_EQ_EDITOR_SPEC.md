# Surgical EQ — Editor UI Spec

For the local Claude Code session. Read alongside `SURGICAL_EQ_HANDOFF.md` and `SURGICAL_EQ_DESIGN.md`. This replaces the `PlaceholderEditor` in `SurgicalEqProcessor.cpp` with the real curve/analyzer editor. All new-files work — no ChainHost/session-b-state collision. Build/iterate locally against `EchoJay_VST3` (the placeholder already builds).

## Deliverable & files
- New `Source/SurgicalEqEditor.{h,cpp}` — a `juce::AudioProcessorEditor` (or a `juce::Component` the editor hosts). Add both to the `EchoJay` **and** `EchoJayLink** `target_sources` blocks in `CMakeLists.txt` (the EQ is meant to run in both; the DSP/wrapper are currently main-target-only — add `SurgicalEqEditor.*`, and while you're there add `SurgicalEqProcessor.*`, `EqEngine.*`, `EqMove.h` to the Link target too so the editor's processor deps resolve there).
- In `SurgicalEqProcessor::createEditor()`, return `new SurgicalEqEditor (*this)` and delete `PlaceholderEditor`.
- Match the house look: pull colours/fonts from `Source/EchoJayLookAndFeel.h` (read it first). Don't invent a new palette.

## Build it in two sub-steps
Land **Step A** first (fully usable EQ), then **Step B**. Commit/push between them.

---

## STEP A — interactive curve editor (no analyzer yet)

### Processor surface it uses (all already exist, message-thread safe)
- `int SurgicalEqProcessor::kNumBands`
- `BandSpec getBand(i)` / `void setBand(i, spec)` — read/write a band; `setBand` publishes to the engine.
- `juce::String applyEqBands(var)` / `var currentEqBandsVar()` — not needed for manual editing but available.
- `void setBypassed(bool)` / `bool isBypassed()`; `void setSoloBand(int)` / `int getSoloBand()`.
- `EqEngine& getEngine()` → `getMagnitudeResponse(const float* freqs, float* magsDb, int n)` for the total curve, and `getBandMagnitudeResponse(i, …)` for a single band's curve.
- `BandSpec` fields (real units): `enabled, type (BandType), freqHz, gainDb, q, slopeDbPerOct, dynamic, thresholdDb, rangeDb, attackMs, releaseMs`. `BandType`: Bell, LowShelf, HighShelf, Notch, HighPass, LowPass.

### Layout (default size 640×420; must resize gracefully — it embeds in a clip rect when hosted)
- **Graph area** (fills most of the view): the EQ curve plotted over a log-frequency x-axis (20 Hz–20 kHz) and a linear dB y-axis (default ±18 dB, with a toggle or auto-expand to ±24). Faint gridlines at decade freqs (100/1k/10k) and dB lines (±6/±12).
- **Band nodes**: one draggable handle per enabled band at (freqHz, gainDb). For Notch/HP/LP (no gain) place the node on the 0 dB line at its freqHz. Selected band is highlighted; its individual curve is drawn faintly under the total curve.
- **Bottom control strip** for the selected band: type selector, numeric freq/gain/Q (editable text or drag-boxes), a Dynamic toggle that reveals threshold/range/attack/release when on, and per-band enable + solo buttons.
- **Global**: a Bypass toggle; an "A" analyzer toggle (wired in Step B).

### Coordinate mapping (put these in one place, reuse for hit-testing and drawing)
- `x = width * (log10(f/20) / log10(20000/20))`; inverse `f = 20 * 10^( (x/width) * log10(1000) )`.
- `y = height * (1 - (gainDb + range) / (2*range))` with `range` = current dB scale (18 or 24).

### Interactions
- **Drag node**: horizontal = freqHz (clamp 20–20k), vertical = gainDb (clamp ±range). Call `setBand` live during drag.
- **Q**: mouse-wheel over a node (or vertical drag with a modifier). Clamp ~0.1–40 for bells/notch. Show a Q readout.
- **Add band**: double-click empty graph area → allocate the lowest disabled band, default type Bell at the clicked freq/gain, select it. (Use the same lowest-free rule as `EqMove`.)
- **Remove/disable**: double-click a node, or a delete key on the selected band → `setBand` with `enabled=false`.
- **Select**: single-click a node.
- **Solo**: press-and-hold (or a toggle) → `setSoloBand(i)`; releasing → `setSoloBand(-1)`.
- **Type / dynamic / numeric edits** in the strip → `setBand`.

### Curve drawing
- Sample the total response on a per-pixel (or every-2px) log-freq grid: build a `float freqs[N]` across the width, call `getEngine().getMagnitudeResponse(freqs, mags, N)`, map each `mags[i]` dB to y, stroke a path. Redraw on a 30 Hz `juce::Timer` and on any edit.
- Draw the selected band's own curve with `getBandMagnitudeResponse` at lower opacity.

### Acceptance for Step A
Manual EQing works end to end in a DAW: add/drag/remove bands, change type, set Q, toggle dynamic and its params, bypass, solo — the curve tracks and the audio changes accordingly. No analyzer yet.

---

## STEP B — analyzer overlay + dynamic metering

### Processor plumbing to add (small, self-contained)
Add a lock-free pre-EQ audio tap to `SurgicalEqProcessor` so the editor can draw a live spectrum:
- Member: a mono ring buffer (e.g. `std::array<float, 4096>` + `juce::AbstractFifo`, or a single-producer atomic write index). In `processBlock`, **before** `engine_.process(...)`, downmix the input block to mono and push it into the ring. Real-time safe: no locks, no allocation.
- Public: `int readAnalysis(float* dest, int maxSamples)` (or expose the fifo) for the editor to drain on the message thread.

### Editor analyzer
- On the 30 Hz timer, drain the latest ~2048 samples, apply a Hann window, run `juce::dsp::FFT` (order 11), compute magnitude in dB, and map bins onto the same log-freq x-axis. Draw as a filled translucent spectrum **behind** the EQ curve. Smooth with a per-bin decay (e.g. `mag = max(newMag, mag*0.85)`) so it's readable.
- Toggle with the "A" button. Default on when hosted, but cheap to leave off.

### Dynamic metering
- For each dynamic band, poll `getEngine().getBandDynamicGainDb(i)` on the timer and draw the live gain-reduction (e.g. a small moving fill from the node down/up, or a dB readout on the node). This makes the de-ess/resonance action visible.

### Acceptance for Step B
Live input spectrum sits behind the curve on the same frequency grid; dynamic bands show their real-time action. Analyzer toggle works; CPU stays sane (FFT on the message-thread timer, not the audio thread).

---

## Constraints (unchanged)
- Keep `EqEngine`/`EqMove` JUCE-free and their g++ tests green (`test/eq_engine_test.cpp`, `test/eq_move_test.cpp`).
- No APVTS; the editor reads/writes via the processor's typed methods.
- Don't touch `ChainHost` or other session-b-state files — the editor is self-contained.
- Match `EchoJayLookAndFeel`. Keep the default view ≤ ~640×420 so it embeds cleanly when hosted as a slot.
- Commit/push to `feat/surgical-eq` after Step A and again after Step B, with the project's commit trailer.
