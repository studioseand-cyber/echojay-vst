# EchoJay Devices — Depth Pass (pro features & modes, all dialable)

Goal: take every built-in from "works" to "pro." Each device gains the character
**modes** and pro controls its category is expected to have — and because every one
rides the existing `ParamSchema` (+ the `choices` selector the Harmonic cluster added),
**every mode and knob is dialable by the model by construction.** A compressor `mode`
is set by `{"params":{"mode":"opto"}}` exactly like any other param; the advertisement
teaches it automatically.

The through-line: the single biggest upgrade almost everywhere is a **`mode`/`character`
choice param** — one selector that reshapes the DSP's feel — plus a few category-standard
pro controls (sidechain filters, lookahead, ducking, detection). Modes are cheap to add
(a switch in the core) and enormously raise perceived quality and dialability.

Keep JUCE-free cores + g++ tests green; every new param gets a schema entry with a
model-facing description (the advertisement is the backend's teaching, so write it well).

---

## Dynamics
**Compressor** — `mode` (choices): `clean` (VCA, transparent/punchy), `glue` (bus, slow
gentle), `punch` (FET/1176-ish, fast aggressive with drive), `smooth` (opto/LA-2A, program-
dependent release). Mode reshapes attack/release curves, knee, and adds subtle harmonic
character. Plus: `sc_hpf_hz` (sidechain high-pass so bass doesn't pump), `lookahead_ms`,
`auto_release` (bool), `detector` (peak|rms), `stereo_link` (%), `range_db` (max GR).
**Limiter** — `mode`: `transparent` | `punchy` | `clip` (hard-clip ceiling). `true_peak`
(bool), `sc_hpf_hz`. **Gate** — `mode`: `gate` | `duck`; `sc_hpf_hz`/`sc_lpf_hz` (frequency-
selective triggering), `lookahead_ms`. **Expander** — `sc_hpf_hz`, `lookahead_ms`.
**De-Esser** — `auto_threshold` (bool), keep wide/split + listen. **4-Band** — per-band
`mode` + a global `detector`.

## Harmonic
**Saturation** — keep the 4 curves as `type`; add `emphasis` (even|odd|both — which harmonics
dominate), `pre_tilt`/bias, `hpf_hz` (don't saturate the sub), `oversample` (quality: 2x|4x|8x).
**Tape** — add machine `mode`: `studio` (clean, hi speed) | `vintage` (softer, more wow) |
`cassette` (narrow, noisy); plus `hiss` and `crosstalk`. **Exciter** — expand `mode` with
`odd`/`even` character; `mix` per band.

## Time
**Reverb** — `algorithm` (choices, the marquee add): `room` | `hall` | `plate` | `spring` |
`ambience` — each a different network/tuning. Plus `duck` (sidechain from dry, so reverb ebbs
under the source), `diffusion` (%), `gate` (gated-reverb bool + time). **Delay** — `mode`:
`digital` (clean) | `tape` (saturated, wow in the loop) | `analog`/`bbd` (dark, gritty) |
`pingpong`; plus `duck` (%), `diffusion` for a smeared/reverby delay.

## Modulation
**Chorus** — `mode`: `classic` | `ensemble` (multi-voice lush) | `dimension` (fixed, no LFO,
wide). **Phaser** — `mode`/`vintage` character + `stereo_spread`. **Tremolo / Auto Pan** —
add `harmonic` and `random`/`sample-hold` shapes to the waveform choice; Tremolo `mode`:
`sine` | `optical` | `bias` (harmonic).

## Stereo
**Stereo Width** — `mode`: `full` | `multiband` (independent low/mid/high width). **Stereoizer**
— `mode`: `haas` | `comb` | `dimension` (chorused widen). Both keep exact mono fold-down.

## EQ (the parked P2–P5, same programme)
Per-band `channel` (stereo|mid|side|left|right), `eq_settings` (output/auto-gain, phase_mode
zero|linear), `eq_action` resonance-hunt, `eq_preset`. Already specced in
SURGICAL_EQ_ENHANCEMENTS.md — folds into this pass.

## Utility
**Gain** — `mode`: `stereo` | `mid_side` (independent M/S gain); `mono` (bool sum), `phase`.

---

## Build approach
Same worktrees + discipline (PARALLEL_SESSIONS.md). One session per category, each:
extend its JUCE-free core with the mode switch(es) + new params (g++-tested), add the
ParamSchema entries with model-facing descriptions, wire the editor controls (a MODE selector
+ the new knobs, in the existing look; modes can hide/show relevant knobs). Cluster sessions
BUILD + self-test only — never install. Integration merges by hand, one at a time.

Order suggestion (impact first): **Dynamics modes** → **Reverb/Delay modes** → **Saturation/Tape
depth** → **Modulation modes** → **Stereo/Gain/EQ**. Each is independently shippable.

## Dialability acceptance (per device)
Every new mode/knob appears in the `[AVAILABLE BUILTINS]` advertisement with a clear
description, is settable via `settings_structured.params` by name (choices resolve by label
AND index), clamps/validates, and round-trips through state. i.e. "make the compressor punchy
and high-pass the sidechain at 120" → `{"params":{"mode":"punch","sc_hpf_hz":120}}` lands exactly.
