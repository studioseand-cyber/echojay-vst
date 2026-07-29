# EchoJay Built-in Devices — Visualization Plan

Goal: every device feels alive, matching the bar the EQ set. Each category gets a
signature visualization users expect from a pro plugin, drawn in the EchoJay look,
placed in `DeviceEditorBase`'s content area above/around the existing dials. The
visuals are READ-ONLY — they don't touch the dialable contract, they make dialing
*legible* (you watch the AI's move land).

## The key simplification: most of it is analytic
A visualization needs one of three data sources. Knowing which keeps the DSP work small:

- **Analytic (no processor change):** the picture is a pure function of the param
  values the editor already reads via `getParamValue`. Draw it directly.
  → dynamics transfer curve, LFO waveform, waveshaper curve, delay taps, reverb decay.
- **One float tap (tiny):** a single lock-free float the editor polls on its timer,
  same contract as `DynamicsCore::gainReductionDb()` / the EQ's band meter.
  → GR meter (exists), input-level dot, LFO playhead phase, I/O level meters.
- **A sample/spectrum ring (small, only where needed):** a lock-free ring the editor
  reads, same pattern as the EQ analyzer.
  → stereo goniometer (L/R sample ring), harmonic bars (output spectrum).

Only the goniometer and the harmonic bars need real signal taps; everything else is
analytic or a single float.

## Shared visualization library (`Source/viz/`) — build once, reuse
Reusable `juce::Component`s styled with `EchoJayDeviceLookAndFeel`, each taking its
data via a simple setter and repainting on a change threshold (the GR-meter contract):

- `TransferCurveView` — input→output plot with threshold/ratio/knee/range; optional live dot.
- `Goniometer` — Lissajous vectorscope from an L/R sample ring + a correlation bar.
- `LfoScopeView` — one LFO cycle (sine/tri/square/saw) with depth, a moving playhead dot, L/R phase.
- `WaveshaperView` — the saturation curve with the signal's current level riding it.
- `HarmonicBars` — 2nd/3rd/4th… harmonic magnitudes as bars.
- `SweepView` — a moving notch/comb curve for Phaser/Chorus.
- `DelayTapsView` — feedback echoes as decaying vertical taps; ping-pong L/R.
- `DecayView` — reverb energy-decay envelope (early vs late), RT60 from `decay_s`.
- `LevelMeter` — I/O peak/RMS bar (reuse for Gain and as a strip elsewhere).
- `EedGrMeter` — already built (Dynamics); moves into `viz/` unchanged.

Small tap helper for the signal-fed ones: a lock-free `VizTap` (float or short ring)
the processor writes from `processBlock` and the editor reads on a timer — the exact
contract the EQ analyzer and GR meter already use, factored out so a device adds a tap
in two lines.

## Signature visualization per category
- **Dynamics** (6) — `TransferCurveView` (threshold/ratio/knee/range, analytic) + `EedGrMeter`
  + a live input-level dot riding the curve (one float tap). 4-Band: four stacked mini
  curves + per-band GR. The single biggest upgrade.
- **Stereo** (2) — `Goniometer` + correlation bar (L/R ring tap). You watch the image widen.
- **Modulation** (4) — `LfoScopeView` with a moving playhead (LFO phase float tap); Chorus/Phaser
  add `SweepView` for the moving notches.
- **Harmonic** (3) — `WaveshaperView` (analytic curve + level dot) + `HarmonicBars` (spectrum tap).
- **Time** (2) — Delay: `DelayTapsView` (analytic). Reverb: `DecayView` (analytic).
- **Utility** — Gain: `LevelMeter` I/O (float tap). Phase Invert: stays minimal (a small
  correlation dot at most).

Editors grow their default height to seat the viz above the dials; keep the inline-hosting
contract (survive being laid out smaller — the viz is the first thing to shrink).

## Build approach (same worktrees, now with the tooling + discipline)
**Phase V0 — foundation (SOLO, blocks the rest):** build `Source/viz/` (all shared
components), the `VizTap` helper, and prove BOTH data paths end-to-end by wiring the
`TransferCurveView`+GR+dot into the **Compressor** (analytic + float tap) and the
`Goniometer` into **Stereo Width** (ring tap). Relax the registry test's exact editor-size
assertions to sane ranges (visuals change default sizes across categories — a hard-coded
size is a merge magnet). Merge V0 before fanning out.

**Phase V1 — parallel per category** (one worktree each, off the merged V0):
Dynamics · Stereo · Modulation · Harmonic · Time+Utility. Each drops its signature view(s)
into its devices' editors and adds any needed tap. Cluster sessions BUILD + self-test only
(harness + pluginval on their own artifact) — **never install** (PARALLEL_SESSIONS.md rule).
Integration merges happen one at a time from `~/echojay-vst` via `install_local.sh`.

## Rules carried over (hard-won)
- Cluster sessions never install to the system; only the integration folder installs.
- Confirm the loaded build with `which_build_is_installed.sh` before believing a DAW result.
- A cluster must merge the integration branch before it is ever installed/DAW-tested.
- The editor-paint regression harness must construct + paint every device's editor
  (already does) — a new viz that crashes on paint fails a test, not a DAW.
- Viz is read-only: no change to `applyStructured`, ParamSchema, or the advertisement.

## Acceptance
Each category: open a device, see its signature visualization drawn correctly and moving
with the signal/params; dial a param (by hand or via a `params` move) and watch the picture
track it. No change to how anything dials.
