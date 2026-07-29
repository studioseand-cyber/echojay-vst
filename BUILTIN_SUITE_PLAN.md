# EchoJay Built-in Effects Suite — Parity Plan

Goal: EchoJay ships its own built-in effects covering **at least** the
competitor's set, every one **fully dialable by the AI** (a knob a human can
turn is a knob the model must be able to set exactly), and every one wearing
the **surgical-EQ look** (EchoJay logo, filmstrip dials with values underneath,
dark Pro-Q aesthetic, inline hosting in the chain). Built to be produced by
**several Claude Code sessions in parallel** on one Mac without colliding.

## The competitor set (20) vs what we have

| # | Device | Cluster | DSP cost | Status |
|---|---|---|---|---|
| 1 | 8-Band EQ | EQ | — | **Have it** (surgical EQ exceeds this) |
| 2 | Gain (level/pan) | A Utility | trivial | build |
| 3 | Phase Invert | A Utility | trivial | build |
| 4 | Stereo Width | B Stereo | low | build |
| 5 | Stereoizer | B Stereo | low | build |
| 6 | Tremolo | C Modulation | low | build |
| 7 | Auto Pan | C Modulation | low | build |
| 8 | Chorus | C Modulation | med | build |
| 9 | Phaser | C Modulation | med | build |
| 10 | Compressor | D Dynamics | med | build |
| 11 | Gate | D Dynamics | med | build |
| 12 | Expander | D Dynamics | med | build |
| 13 | Limiter | D Dynamics | med | build |
| 14 | De-Esser | D Dynamics | low* | build (*reuses EQ dynamic bell) |
| 15 | 4-Band Compressor | D Dynamics | high | build |
| 16 | Saturation | E Harmonic | med | build |
| 17 | Tape | E Harmonic | med | build |
| 18 | Exciter | E Harmonic | med | build |
| 19 | Delay | F Time | med | build |
| 20 | Reverb | F Time | high | build |

**19 to build.** Clustered by **shared DSP core** so a cluster builds its core
once and hangs several faces off it — that's the reuse that makes 19 tractable.

---

## Architecture — build once, reuse for all 19

### 1. Self-registering device registry (kills parallel-merge conflicts)
Today the built-in path is EQ-specific (`kBuiltinEqName`, `builtinEqDescription`,
`isBuiltinEqName`, `isBuiltinEqSlot`, one processor factory). Generalize it into a
registry so a new device is **self-contained in its own files and registers
itself** — no edit to any shared list:

```cpp
struct BuiltinDevice {
  juce::String name;             // "EchoJay Compressor"
  juce::String category;         // "Dynamics" (for the add-menu grouping)
  std::function<std::unique_ptr<juce::AudioProcessor>()> create;
  ParamSchema  schema;           // dialable contract (see §3) — feeds advertise + validate
};
// Each device .cpp ends with:  static BuiltinDeviceRegistrar _reg { makeCompressorDevice() };
```

- `builtinDeviceNames()` returns `registry().names()` instead of a literal list.
- `builtinDescriptionFor(name)` builds the synthetic `PluginDescription` from the
  registry entry (replaces `builtinEqDescription`).
- `isBuiltinSlot(i)` / dispatch key off `registry().contains(name)` — no per-device
  `isBuiltinEqSlot` clones.
- The `[AVAILABLE BUILTINS]` advertisement (`EchoJayAPI::buildPluginInjection`) and
  the add-menu are **generated from the registry**, so a new device shows up in the
  menu and the AI feed automatically.

**Result:** two sessions building two devices touch **zero** common lines — each
adds its own files + its own registrar. Merge conflicts approach nil.

### 2. Shared look — `EchoJayDeviceLookAndFeel` + `DeviceEditorBase`
Extract from `SurgicalEqEditor` (this is the "base the look off the EQ" ask):
- `EchoJayDeviceLookAndFeel`: the dark palette, the **filmstrip rotary** (mix-knob
  style) with value-underneath, the section/button styling, TYPE-selector alignment.
- `DeviceEditorBase`: header with **EchoJay logo top-left**, device title, bypass,
  a content area, and the **inline chain-hosting** sizing contract (no floating
  window). Every device editor subclasses this and only lays out its own controls.
- Where a device wants a spectrum/meter (dynamics GR meters, EQ analyzer), reuse
  the analyzer component with its separate-axis calibration already solved.

A new device's editor is then ~"place N filmstrip dials + a meter" — the identity
is inherited, not re-authored.

### 3. Universal dialable param contract (the north star, made uniform)
The EQ dials via `settings_structured.eq_bands`. Generalize so **every** device
dials the same way. Two shapes under `settings_structured`:

- **Structured devices** (EQ, 4-band comp) keep their array form (`eq_bands`,
  `comp_bands`) — arrays of sub-objects with merge semantics.
- **Everything else** uses a flat, unit-carrying param map:
  ```jsonc
  settings_structured: {
    "params": {                 // canonical id → value in real-world units
      "threshold_db": -18, "ratio": 4, "attack_ms": 10,
      "release_ms": 120, "makeup_db": 3, "mix": 100
    }
  }
  ```
Each device **publishes a `ParamSchema`** — for every param: canonical id, unit,
min/max/default, and a one-line description. That single schema drives three things
at once: (a) the backend **teaches** it to the model, (b) the server **validates/
clamps** the move against it, (c) the processor **maps** id→knob on apply. Merge
semantics: any param absent = leave as-is (so "make it punchier" can send just
`attack_ms`/`release_ms`).

So dialability is not per-device bespoke work — building a device's schema **is**
building its dialability. Nothing is clickable-but-not-dialable by construction.

### 4. Shared DSP cores (JUCE-free, g++-tested, one per cluster)
- **Dynamics core:** detector (peak/RMS), stereo-linked envelope follower, gain-
  computer (threshold/ratio/knee/range), attack/release. Compressor, Gate,
  Expander, Limiter, De-Esser, and each band of the 4-band all = a face on this.
- **Modulation core:** a shared LFO (rate, depth, phase, waveform) + wet/dry.
  Tremolo (amplitude), Auto Pan (pan), Chorus/Phaser (delay/allpass modulated).
- **Harmonic core:** oversampled waveshaper with selectable curves + tone tilt.
  Saturation, Tape (adds wow/flutter + head bump), Exciter (band-split + shape).
- **Stereo core:** M/S matrix + width/rotation. Stereo Width, Stereoizer.
- **Time core:** fractional delay line + feedback + filtering. Delay, and Reverb
  (network of delays/allpasses) build on it.
- **Utility:** Gain (level/pan/trim), Phase Invert — no shared core needed;
  perfect **first devices to prove the framework end-to-end**.

Every core keeps the EQ discipline: no JUCE, a g++ unit test in `test/`, block-rate
smoothed params, lock-free publish.

---

## Wave plan (what runs when, and in parallel)

### Wave 0 — Framework + look + proof (SOLO, blocks the rest) — 1 session
1. Generalize the built-in path into the self-registering **registry** (§1).
2. Extract **`EchoJayDeviceLookAndFeel` + `DeviceEditorBase`** from the EQ (§2);
   refactor `SurgicalEqEditor` onto them (proves the base is sufficient).
3. Define the **`ParamSchema` + `params` contract** (§3) and the registry→advertise
   + registry→add-menu generation.
4. Ship **Gain** and **Phase Invert** through the whole pipe (registry → menu →
   host → `applyStructured(params)` → advertise → editor on the shared look) as the
   proof the pattern works. Add a **device template** (skeleton processor/editor/
   engine + registrar) new devices copy.
5. Backend: generalize the EQ contract doc to the multi-device `params` contract;
   the registry advertisement already gates capability per §1.

Merge Wave 0 before fanning out. (Coordinate with the EQ-enhancements branch:
both touch ChainHost's apply call site — land EQ Phase 1 first, then Wave 0 rebases
its `applyStructured` dispatch onto it. After Wave 0, the EQ enhancements P2–P5
become "just another device's schema growth.")

### Wave 1 — parallel fan-out (after Wave 0 merges) — up to 5 sessions
One session per cluster, each on its own branch `feat/builtin-<cluster>`,
self-registering, merged when green:

- **Session A — Stereo (B):** Stereo Width, Stereoizer.
- **Session B — Modulation (C):** shared LFO core → Tremolo, Auto Pan, Chorus, Phaser.
- **Session C — Dynamics (D):** shared dynamics core → Compressor, Gate, Expander,
  Limiter, De-Esser (reuse EQ dynamic bell), then 4-Band Compressor.
- **Session D — Harmonic (E):** oversampled shaper core → Saturation, Tape, Exciter.
- **Session E — Time (F):** delay-line core → Delay, then Reverb.

Each session: build the cluster core + g++ test **first**, then the faces, each
face = engine params + `ParamSchema` + editor on `DeviceEditorBase` + registrar +
backend teaching block. Push, rebuild, reinstall the AU, report.

### Wave 2 — integration & polish — 1 session
Full-suite pass: add-menu categories, one end-to-end backend deploy teaching all
device schemas, an AI acceptance batch ("build me a vocal chain" → places EchoJay
EQ + Comp + De-Esser + Reverb and **dials** each), latency reporting (Reverb/Tape),
CPU check, preset bank per device (optional).

---

## Rules that keep parallel work safe
- **Self-registration only** — a device never edits a central list, menu, or
  advertisement; it registers itself. The registry generates all three.
- **One device = its own files** (`EedXxxEngine.{h,cpp}`, `EedXxxProcessor.{h,cpp}`,
  `EedXxxEditor.{h,cpp}`) + one registrar line in its own `.cpp`. Nothing shared
  is edited, so branches merge clean.
- **Schema = dialability**: no knob ships without a `ParamSchema` entry. That is
  the definition of done, and it's what the backend teaches + validates.
- **JUCE-free cores + g++ tests stay green**, same discipline as `EqEngine`.
- **Ship both halves or neither**: a device's client side and its backend teaching
  land together; the registry advertisement is the single capability signal.
- CMake: add new `Source/Eed*.cpp` to the target's source list (the one shared
  file everyone appends to — keep it alphabetized to minimize conflicts, or glob).

## Acceptance
- Every device: appears in the add-menu, hosts inline in the chain wearing the
  EchoJay look, and **dials exactly** from a `settings_structured.params` (or array)
  move — verifiable on its readouts.
- Suite: "build a mastering chain" / "make this vocal sit" → the model reaches for
  EchoJay's own devices and sets their params precisely, no anchor tables involved.
