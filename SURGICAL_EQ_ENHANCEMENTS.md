# Surgical EQ — Enhancements & Dialability Master Spec

Day-2 design. Scope: the six parked enhancements —
**resonance-hunt helper, mid/side per band, linear-phase mode, presets,
output/auto-gain, note-name tooltip** — built so that **every one of them is
dialable by the AI**, not just clickable in the UI. That is the north star:
a knob a human can turn is a knob the model must be able to turn too.

Read `SURGICAL_EQ_BACKEND_SPEC.md` first for the existing `eq_bands` contract;
this doc extends it. Keep `EqEngine`/`EqMove` JUCE-free and their g++ tests green.

---

## 0. The dialability principle (why the schema grows)

Today a move rides as a bare array: `settings_structured.eq_bands = [ {band…}, … ]`.
That covers per-band static + dynamic moves and nothing else. Four of the six
enhancements are **not** per-band-parameter changes:

| Enhancement | What it actually is | Can `eq_bands` express it? |
|---|---|---|
| Output / auto-gain | a **device-global** setting | no |
| Linear-phase mode | a **device-global** mode (adds latency) | no |
| Mid/side per band | a **per-band routing** attribute | needs a new band field |
| Presets | a **named bundle** of bands + settings | no |
| Resonance-hunt | an **imperative action** ("go find and tame them") | no |
| Note tooltip | UI-only, but the **note→freq** map should be dialable ("tune 400→A4") | partially |

So the move shape grows from a bare array into a **backward-compatible object**.
The rule the client enforces: **if `settings_structured` is an array, treat it as
`eq_bands` (legacy); if it's an object, read `eq_bands` / `eq_settings` /
`eq_action` / `eq_preset` off it.** No existing move breaks.

### The extended move object (authoritative target shape)

```jsonc
settings_structured: {
  // (1) per-band moves — unchanged array, ONE new optional field: channel
  "eq_bands": [
    { "type":"bell", "freq_hz":400, "gain_db":-3, "q":3,
      "channel":"stereo" }          // NEW: stereo|mid|side|left|right (default stereo)
  ],

  // (2) device-global settings — all optional, merge semantics
  "eq_settings": {
    "phase_mode":  "zero",          // zero (default, minimum-phase) | linear
    "output_db":   0.0,             // -24..+24 trim on the EQ output
    "auto_gain":   false,           // true = compensate loudness change from the bands
    "ms_mode":     false            // true = process/display in Mid/Side
  },

  // (3) an imperative directive the CLIENT executes with DSP it owns
  "eq_action": {
    "type": "tame_resonances",      // the only action in this spec
    "sensitivity": "medium",        // low|medium|high  (how aggressive)
    "range_hz": [200, 8000],        // optional search band
    "max_bands": 4,                 // optional cap
    "dynamic": true                 // true = dynamic bells (musical), false = static notches
  },

  // (4) a named preset the client resolves to bands + settings
  "eq_preset": "vocal-clarity"      // string name from the built-in preset table
}
```

Every field is optional. A turn may send any subset — e.g. just `eq_settings`
to flip linear-phase, or `eq_bands` + `eq_settings.auto_gain` together.

### Client dispatch (one funnel)

Add `applyStructured(const juce::var&)` on `SurgicalEqProcessor` as the single
entry the chain calls (build **and** edit path). It:

1. If the var is an **array** → `applyEqBands(var)` (legacy, unchanged).
2. If an **object** → in this order: resolve `eq_preset` (expands to bands+settings,
   applied first as a base), then `eq_settings` (merge), then `eq_bands` (per-band
   merge, on top of the preset), then `eq_action` (run last, sees the current state).

Order matters: a preset lays the foundation, explicit bands override it, an action
operates on the result. `ChainHost::applyStructuredIfReady` calls this one method
instead of reaching for `eq_bands` directly (one-line change at the call site).

---

## 1. Phase plan (build order)

Foundation first, so later phases have a schema and a funnel to slot into.
Each phase is independently shippable and independently testable.

| Phase | Enhancement(s) | Why here |
|---|---|---|
| **1** | Move-shape upgrade + **output/auto-gain** | Establishes the object schema, the `applyStructured` funnel, and `eq_settings`. Output trim is the simplest global to prove the pipe end-to-end. |
| **2** | **Mid/side per band** | Adds the one new per-band field (`channel`) + the `ms_mode` global. Needs the object schema from P1. |
| **3** | **Resonance-hunt** (`eq_action`) | The highest-value "AI does the work" feature. Reuses dynamic bells already in the engine. Needs the funnel from P1. |
| **4** | **Linear-phase mode** | Most DSP-invasive (latency, FFT convolution). Isolated behind `phase_mode`; nothing else depends on it. |
| **5** | **Presets** + **note tooltip** | Presets are a bundle of everything P1–P4 exposed, so they come last. Note tooltip is a small UI+schema add that rides along. |

If Sean wants a different order, the only hard dependency is **P1 before
everything** (schema + funnel). P2–P5 are independent of each other.

---

## 2. Phase 1 — object schema + output/auto-gain

### DSP (`EqEngine`)
- Add `float outputDb = 0.0f;` applied as a final gain on the summed output.
- Add `bool autoGain = false;`. When on, compute the **magnitude-weighted
  loudness delta** of the enabled static bands (sum of band gains folded through
  a pink-weighting is fine for v1; a measured RMS-match is a later refinement)
  and apply its inverse as makeup, *before* `outputDb`. Keep it JUCE-free and
  smoothed at block rate like the other params. Expose `float autoGainDbApplied()`
  so the UI can show what it did.
- g++ test: a +6 dB bell with `autoGain` on yields output within ~1 dB of unity;
  `outputDb` sums linearly on top.

### Processor
- `applyStructured(var)` funnel per §0.
- `applyEqSettings(var)`: reads `output_db` (clamp ±24), `auto_gain` (bool),
  `phase_mode`/`ms_mode` (parse + store; DSP lands in P2/P4 — store now so the
  schema is stable). Merge semantics: absent key = leave as-is.
- Extend state (`getStateInformation`/`currentEqBandsVar` sibling): persist an
  `eq_settings` object next to `eq_bands`, bump `"v"` to 2, read v1 (no settings)
  as defaults.

### UI (`SurgicalEqEditor`)
- Output dial (filmstrip, same style as the mix knob) at the right of the header,
  value underneath in dB. Auto-gain as a small toggle beside it; when lit, show
  the compensation it's applying as the dial's sub-readout.

### Backend teaching
- Add to `eq_settings` docs: `output_db`, `auto_gain`. Example moves:
  ```json
  {"eq_bands":[{"type":"bell","freq_hz":3000,"gain_db":4,"q":1.2}],
   "eq_settings":{"auto_gain":true}}
  ```
  Guidance: "when a boost would raise level, set `auto_gain:true` so the tonal
  move isn't confounded by loudness; use `output_db` to trim the whole EQ."

---

## 3. Phase 2 — mid/side per band

### DSP (`EqEngine`)
- `kMaxChannels` stays 2. Add a per-band `channel` enum
  `{ Stereo, Mid, Side, Left, Right }`. In `process`, when any band is non-Stereo,
  run an M/S (and/or L/R) split: `mid=(L+R)/2, side=(L-R)/2`, filter each band into
  its routed lane, recombine `L=mid+side, R=mid-side`. Stereo bands process both
  lanes identically (current behaviour). Guard mono input (Side = silence; a
  Side-routed band is a no-op on mono — report it, don't crash).
- `ms_mode` global only changes **display/interaction default** (new bands default
  to Mid or Side per the analyzer's current lane), not routing — routing is
  per band via `channel`. Keep those concepts separate.
- g++ test: a Side-only cut leaves a mono (L==R) signal untouched; an M/S
  round-trip with no active bands is bit-identical to input.

### Processor / schema
- `specFromVar`: read `channel` (tolerant: `m`/`mid`, `s`/`side`, `l`/`left`,
  `r`/`right`, default `stereo`). Add to `BandSpec`. Echo back in
  `currentEqBandsVar`.

### UI
- Per-band `channel` selector (small L/R/M/S segmented control on the band row or
  in the band's context strip). Analyzer gains an M/S view toggle (`ms_mode`):
  splits the trace into mid and side, or overlays them.

### Backend teaching
- Add `channel` to the band schema table + example:
  ```json
  {"eq_bands":[{"type":"bell","freq_hz":8000,"gain_db":3,"q":1,"channel":"side"}]}
  ```
  Guidance: "widen air = boost `side`; tighten a boomy center = cut `mid`;
  de-ess only the center vocal = dynamic bell on `mid`."

---

## 4. Phase 3 — resonance-hunt (`eq_action`)

The marquee dialable feature: the model says *tame the resonances* and the
**client** does the analysis + places the bands. The model doesn't need to know
the frequencies — the plugin finds them from the live signal.

### Client DSP (owned by the processor, runs on a captured buffer)
- Reuse the analysis rings already feeding the analyzer. On an `eq_action` of
  type `tame_resonances`, capture ~1–2 s of PRE signal, run the resonance
  detector: long-window FFT → fractional-octave smoothing → find peaks that
  stand **above the local spectral envelope** by a margin set by `sensitivity`
  (low ≈ 6 dB over envelope, medium ≈ 4, high ≈ 2.5), within `range_hz`,
  up to `max_bands`, spaced ≥ ⅓ octave apart.
- For each peak, place a band:
  - `dynamic:true` (default) → a **dynamic bell** at the peak freq, Q from the
    peak's sharpness, `range_db` negative (compress the resonance only when it
    rings), `threshold_db` from the envelope. Musical, program-dependent.
  - `dynamic:false` → a **static notch** at the peak, Q from sharpness. Surgical,
    always-on.
- Bands are placed through the **same `applyEqMoves` merge** (auto-allocate),
  so a hunt never clobbers hand-dialed bands. Return a summary of what it found.

### UI
- A "Hunt" button in the header (magnifying-glass) runs the same action with the
  UI's current sensitivity setting. A small sensitivity selector (low/med/high)
  and a dynamic/static toggle. Found bands animate in and are fully editable —
  the tool proposes, the user disposes.

### Backend teaching
- Add `eq_action` to the schema with the `tame_resonances` example:
  ```json
  {"eq_action":{"type":"tame_resonances","sensitivity":"medium","dynamic":true}}
  ```
  Guidance: "when the user asks to *tame/remove resonances / ringing / harshness*
  but doesn't name frequencies, emit `eq_action.tame_resonances` and let the EQ
  find them; name `range_hz` if the user localizes it (e.g. 'in the low mids')."

### Test
- g++ test on the detector core (JUCE-free): synthesize a tone + a resonant
  peak; assert the detector returns the peak freq within a tolerance and ignores
  broadband content.

---

## 5. Phase 4 — linear-phase mode

Isolated, latency-introducing. Behind `eq_settings.phase_mode`.

### DSP
- `phase_mode:"linear"` swaps the per-band SVF cascade for an **FFT
  overlap-add convolution** against the EQ's own impulse response (derive the IR
  from the same analytic magnitude the UI already computes; linear phase = zero
  phase target, symmetric IR). Partitioned convolution or a single large block
  as bandwidth allows. `phase_mode:"zero"` (default) is today's minimum-phase path.
- Latency: report the IR half-length via `getTailLengthSeconds()`/
  `setLatencySamples` (the hook already returns 0.0 — wire it here so the host
  compensates). Switching modes re-reports latency.

### Processor / UI / Backend
- Store `phase_mode` in `eq_settings` (P1 already parses/persists it).
- UI: a ZERO/LINEAR toggle in the header; show latency when LINEAR is active.
- Backend guidance: "use `phase_mode:linear` for mastering / parallel / phase-
  critical material where pre-ring is acceptable; keep `zero` (default) for
  tracking and low-latency monitoring."

### Test
- g++: linear path magnitude matches the analytic curve within tolerance; IR is
  symmetric (phase ≈ 0). Verify reported latency == IR center.

---

## 6. Phase 5 — presets + note tooltip

### Presets
- A built-in preset table (JUCE-free data): each preset = `{ eq_bands[], eq_settings }`.
  Start with ~6: `vocal-clarity`, `warm-tape` (tilt), `de-harsh`, `sub-cleanup`
  (HPF), `air-lift` (side high-shelf), `mud-cut`. Resolving a preset **applies its
  bands + settings as the base** (§0 order), so a preset then explicit bands = tweak.
- UI: a preset menu in the header (load; "save user preset" is a later nice-to-have
  — built-ins only for v1).
- Backend: `eq_preset:"name"` in the schema, with the list of names and one-line
  descriptions. Guidance: "reach for a preset as a fast starting point when the
  user's ask matches one; then refine with explicit `eq_bands`."

### Note-name tooltip (dialable note↔freq)
- UI: while dragging a band, show the nearest musical note (e.g. `A4 440 Hz`,
  `+12 cents`) beside the freq readout. Pure display.
- Dialable hook: `specFromVar` already takes `freq_hz`; add tolerant parse of a
  `note` field on a band (`"A4"`, `"C#3"`) → converts to `freq_hz` when `freq_hz`
  is absent. Lets a move say "notch the ringing at G5" precisely.
  ```json
  {"eq_bands":[{"type":"notch","note":"G5","q":8}]}
  ```
- Backend guidance: "if the user names a musical pitch, you may send `note`
  instead of `freq_hz`; the EQ resolves it (A4=440)."

---

## 7. Cross-cutting: keep it all green

- `EqEngine`/`EqMove` stay JUCE-free; every DSP addition gets a g++ test in
  `test/`. Never let `eq_engine_test` / `eq_move_test` go red.
- Every phase is backward-compatible: an old backend that sends a bare `eq_bands`
  array still works (array branch of `applyStructured`).
- Every phase ships **both halves or neither**: the client field/DSP **and** the
  backend teaching line, so a capability is never advertised before it exists.
  The `[AVAILABLE BUILTINS]` advertisement stays the single capability signal;
  if a future phase needs finer gating, advertise a versioned capability token
  in that same block rather than reintroducing version pins.
- State version bumps once (v2 in P1) and stays forward/back compatible.

## 8. Acceptance per phase
- **P1:** "boost 3 k by 4 and keep it level-matched" → bell + `auto_gain:true`,
  output unchanged in loudness. Output dial trims audibly.
- **P2:** "add air to the sides only" → side high-shelf; mono sum untouched.
- **P3:** "tame the harsh resonances" → hunt places dynamic bells on real peaks,
  editable, hand bands preserved.
- **P4:** "make it linear phase for the master" → mode flips, host reports latency.
- **P5:** "start from a vocal clarity preset then cut 300 by 2" → preset loads,
  then the explicit band overrides. "notch the ring at G5" → notch lands on 784 Hz.
