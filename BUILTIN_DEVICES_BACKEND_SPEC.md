# Built-in devices — backend (/api/chat) contract

Generalises `SURGICAL_EQ_BACKEND_SPEC.md` from one device to the whole built-in
suite (`BUILTIN_SUITE_PLAN.md` §3). That doc is still correct for the EQ's
`eq_bands` array; this one adds the **universal `params` path** every other
built-in uses, and describes the new shape of the capability advertisement.

As before: the client half is built and authoritative, and lives in this repo.
The work is in the backend that serves `/api/chat`.

## What changed on the client

The `[AVAILABLE BUILTINS]` block is no longer a bare list of names. It is now
**generated from the device registry**, grouped by category, and each device
carries its own parameter schema. One block therefore teaches the model both that
a device exists and exactly how to dial it:

```
[AVAILABLE BUILTINS — EchoJay's own built-in devices, always available regardless
of installed plugins; use in a chain by exact name. Dial one by putting
real-world values under settings_structured.params using the ids listed; any
param you omit is left as it is. The EchoJay EQ additionally takes its bands as
settings_structured.eq_bands]:

-- EQ --
EchoJay EQ
  EchoJay's own fully-parametric EQ, dialled to EXACT values rather than
  approximated. Prefer it for surgical moves: ...
    output_db (dB, -24..24, default 0) - device output trim, applied after the bands
    auto_gain (on/off, default off) - cancel the loudness change the bands cause, ...

-- Utility --
EchoJay Gain
  Exact level trim and constant-power pan. ...
    level_db (dB, -60..24, default 0) - output level; -60 is silence, 0 is unity
    pan (-1..1, default 0) - stereo position: -1 hard left, 0 centre, +1 hard right ...
EchoJay Phase Invert
  Flips the polarity of either channel independently. ...
    invert_left (on/off, default off) - invert the polarity of the left channel
    invert_right (on/off, default off) - invert the polarity of the right channel
```

**This block is the whole contract.** It is generated from the same `ParamSchema`
objects the client validates and applies with, so the server never has to carry a
hard-coded copy of any device's parameters, and the two cannot drift. A device
added in a later wave appears here automatically — the backend needs no release
to support it, and no version pin on either side.

## The two move shapes

### 1. Flat `params` — every device
```jsonc
{
  "name": "EchoJay Gain",
  "role": "...",
  "settings": "...",
  "settings_structured": {
    "params": { "level_db": -3, "pan": 0.5 }
  }
}
```
- **Canonical ids and real-world units**, exactly as advertised. Never normalised
  0..1 — the whole point of a built-in is that it lands on the number you sent.
- **Merge semantics**: any param omitted is left as it is. So "make it punchier"
  can send only `attack_ms`/`release_ms` without restating the device.
- An id outside the device's schema is **reported, not guessed at** — it comes
  back in the summary as `ignored <id>`, and the slot is marked partial.

#### Discrete params look like numbers, because they are
Some params are enumerations advertised as a plain numeric range with the meaning
spelled out in the description — `shape (0..3)` on Tremolo/Auto Pan,
`sync_division (0..12)`, `voices (1..4)`, `stages (2..12)`. Send the **number**,
not the label: `{"shape": 2}`, never `{"shape": "square"}` (a non-numeric string
is refused, as it should be — see `numberFromVar`). Non-integers are **rounded**,
so `1.6` is shape 2; nothing needs to be pre-snapped server-side.

#### Rate and tempo sync are one interlock
Every Modulation device carries `rate_hz`, `sync` and `sync_division`. While
`sync` is on, `rate_hz` is **ignored** and the division drives the LFO; while it
is off, the division is ignored. Both remain settable at all times, so a move may
set them together in either order. "Make it wobble on eighths" is
`{"sync": true, "sync_division": 8}` — sending only the division leaves a device
whose sync is off unchanged, which is the one way to get a silent-looking no-op
here.

### 2. Array forms — structured devices only
The EQ keeps `settings_structured.eq_bands` exactly as
`SURGICAL_EQ_BACKEND_SPEC.md` documents it (including a bare `[...]` array, which
must keep working forever). A device can carry both in one move: `eq_bands` for
the bands, `params` for the device-global knobs.

Resolution order inside a move is the device's own and is deliberate — for the
EQ: `eq_preset`, then `eq_settings`/`params`, then `eq_bands`, then `eq_action`.

**`comp_bands` — EchoJay 4-Band Compressor.** The second structured device, and
the only other array form:

```jsonc
{
  "name": "EchoJay 4-Band Compressor",
  "settings_structured": {
    "params":     { "crossover3_hz": 6000 },
    "comp_bands": [ { "band": 4, "threshold_db": -24, "ratio": 5 } ]
  }
}
```

- `band` is **1..4**, fixed by the crossovers. Omit it and the entry's **position
  in the array** is its band, so four entries in order address bands 1-4. A band
  outside 1..4 is reported, never wrapped or clamped onto a real band.
- Per-entry keys: `threshold_db`, `ratio`, `attack_ms`, `release_ms`, `knee_db`,
  `makeup_db`, `bypass` (the short spellings `threshold`/`attack`/`release`/
  `knee`/`makeup`/`gain_db` are accepted too).
- **Merge semantics differ from `eq_bands`, deliberately.** An `eq_bands` entry
  REPLACES the band it targets; a `comp_bands` entry MERGES into it. An EQ band is
  a free-floating object you place, so a partial set would leave half of a
  previous shape behind; a compressor band is a fixed slot the crossovers define,
  so "make band 4 faster" must not reset its threshold.
- **`params` is resolved before `comp_bands`** in this device, because the
  crossovers change what each band covers: a move that widens band 4 and then
  sets its threshold has to set the threshold of the band it just defined.
- Every per-band knob is **also a flat id** — `band4_threshold_db`,
  `crossover2_hz` — advertised in the schema like any other param. `comp_bands` is
  a convenience over the same contract, not a second one; both land on the same
  knob through the same validation. A backend that only ever emits flat `params`
  is fully capable of driving this device.

## Backend changes

1. **Parse the advertisement, don't hard-code it.** Read device names, categories
   and param schemas out of the `[AVAILABLE BUILTINS]` block, and offer a
   built-in only when it appears there. That is what makes the capability gate
   the client's statement rather than a version guess.
2. **Teach the schema.** Each param line already carries id, unit, range, default
   and a one-line description. Pass them through; they are written to be read by
   the model.
3. **Validate and clamp server-side** against the advertised min/max before the
   move ships, and drop ids the device did not advertise. The client clamps too
   (and reports what it ignored), but a move should be sane before it leaves.
4. **Prefer built-ins for exact work.** They dial precisely; third-party plugins
   go through the approximate anchor-table path. Reach for `EchoJay Gain` for
   level/pan staging, `EchoJay Phase Invert` for polarity problems, the
   `EchoJay EQ` for surgical tonal moves, for movement `EchoJay Tremolo`
   (level), `EchoJay Auto Pan` (position), `EchoJay Chorus` (thickening/width),
   `EchoJay Phaser` (swirl), and the Dynamics cluster — `EchoJay Compressor`,
   `Gate`, `Expander`, `Limiter`, `De-Esser`, `4-Band Compressor` — for anything
   about level over time. Each device's `summary` line says when to pick it over
   its neighbours; that text is written for the model and needs no gloss.
   Two notes the schema cannot carry on its own:
   - `EchoJay De-Esser`'s `listen` is a **monitoring** switch: on, the device
     outputs the detector's band instead of the result. Never leave a move with
     it on.
   - `EchoJay Limiter`'s `lookahead_ms` adds **latency**, which the client
     reports to the DAW. It is the right default on a master, and the wrong one
     when a chain is being monitored live.
5. **Booleans** may be sent as JSON `true`/`false` or as `1`/`0`; the client
   accepts both, plus the strings `"on"`/`"off"`. Prefer real JSON booleans.

## Character MODES — the depth pass (read this; it is most of the expressive range)

Nearly every device now carries a **`mode`/`algorithm`/`type` choice param** that
reshapes its character. These are `choices` params: send the **name** (preferred,
e.g. `"punch"`) or the index. Unknown labels are skipped, never guessed. The
advertisement lists each device's exact choices and describes them — that text is
authoritative and generated from the same table the client resolves, so it can
never drift. Highlights the model should reach for:

- **Compressor `mode`**: `clean` (VCA, transparent) · `glue` (bus, slow/gentle) ·
  `punch` (FET, fast/aggressive) · `smooth` (opto, programme-dependent). Plus
  `sc_hpf_hz` (stop bass pumping the compressor — reach for ~80-120 on a mix bus),
  `detector` (peak|rms), `auto_release`, `stereo_link`, `lookahead_ms`, `range_db`.
- **Limiter `mode`**: `transparent` · `punchy` · `clip` (hard ceiling). `true_peak`.
- **Gate `mode`**: `gate` · `duck` (the same controls, inverted — ducking is a mode,
  not a separate device). `sc_hpf_hz`/`sc_lpf_hz` for frequency-selective triggering.
- **Reverb `algorithm`**: `room` · `hall` · `plate` · `spring` · `ambience`.
  `decay_s` stays the RT60 in all five, so algorithm changes CHARACTER, not length.
  Plus `duck` (tail ebbs under the dry signal — the move for a vocal reverb that
  doesn't wash) and `diffusion`.
- **Delay `mode`**: `digital` (clean) · `tape` (saturating, wow/flutter, each repeat
  darker) · `analog` (BBD, darker and grittier still) · `pingpong`. Plus `duck`,
  `diffusion`.
- **Saturation**: `type` (tube|tape|diode|soft) AND `emphasis` (`even` = warm/tubey
  2nd · `odd` = aggressive/console 3rd · `both`) — emphasis is the biggest character
  control after type. Plus `hpf_hz` (drive the mids without mushing the bass),
  `bias`, `oversample` (2x|4x|8x, affects reported latency).
- **Tape `mode`**: `studio` · `vintage` · `cassette` (band-limited, noisiest). Plus
  `hiss` (gated to signal) and `crosstalk`.
- **Exciter `mode`**: `tube` · `tape` · `odd` · `even`, plus `focus`.
- **Tremolo `mode`**: `sine` · `optical` · `bias` (harmonic tremolo — lows and highs
  counter-phase; far richer than level tremolo). **Chorus `mode`**: `classic` ·
  `ensemble` (lush multi-voice) · `dimension` (wide, no pitch wobble — note it
  ignores rate/depth). **Phaser `mode`**: `modern` · `vintage` · `stereo`.
  **Auto Pan `mode`**: `linear` · `constant_power` · `binaural`. Waveform `shape`
  now includes `harmonic` and `random` (sample-and-hold) alongside sine/tri/square/saw.
- **Stereo Width `mode`**: `full` · `multiband` (independent `width_low`/`width_mid`/
  `width_high` around `xover_low_hz`/`xover_high_hz` — narrow lows, wide highs).
  Plus `rotation`. **Stereoizer `mode`**: `haas` · `comb` · `dimension`.
- **Gain `mode`**: `stereo` · `mid_side` (`mid_db`/`side_db`). Plus `mono`,
  `phase_left`, `phase_right` — Gain covers the common utility jobs in one device.

**Every mode's default is the previously-shipped behaviour**, so omitting `mode` is
always safe and never changes an existing chain.

## The EQ's full object move (P2-P5 shipped)

`settings_structured` for `EchoJay EQ` may be a bare `eq_bands` array (forever
supported) or an object resolved in this order: **`eq_preset` → `eq_settings` →
`eq_bands` → `eq_action`**. A preset lays a base, explicit bands refine it, an
action runs last on the result.

```jsonc
{
  "eq_preset": "vocal-clarity",                       // base (see advertised list)
  "eq_settings": { "phase_mode":"linear", "ms_mode":false,
                   "output_db":-1, "auto_gain":true },
  "eq_bands":  [ { "type":"bell","freq_hz":300,"gain_db":-2,"q":1.2,
                   "channel":"mid" },                 // NEW: stereo|mid|side|left|right
                 { "type":"notch","note":"G5","q":8 } ],   // NEW: note instead of freq_hz
  "eq_action": { "type":"tame_resonances","sensitivity":"medium",
                 "range_hz":[200,8000],"max_bands":4,"dynamic":true }
}
```

- **`channel`** routes a band to a lane: boost `side` for air/width, cut `mid` for a
  boomy centre, de-ess only the centred vocal with a dynamic bell on `mid`. A
  side-routed band on mono material is a reported no-op.
- **`note`** ("A4", "C#3") resolves to a frequency when `freq_hz` is absent — use it
  when the user names a pitch ("notch the ring at G5" → 784 Hz).
- **`phase_mode: linear`** is for mastering / parallel / phase-critical work; it adds
  reported latency (~53 ms @ 48k), so keep the default `zero` for tracking and live
  monitoring. Say so when choosing it.
- **`eq_action.tame_resonances` is the feature to reach for when the user asks to
  tame resonances / ringing / harshness WITHOUT naming frequencies.** The plugin
  analyses the live signal, finds the peaks itself and places dynamic bells (or
  static notches with `dynamic:false`), without clobbering hand-dialled bands. Narrow
  it with `range_hz` if the user localises it ("in the low mids"). This is the one
  move where the model does not need to know the frequency at all.
- Presets: the advertised list is generated from the client's own table
  (vocal-clarity, warm-tape, de-harsh, sub-cleanup, air-lift, mud-cut). Use one as a
  fast start, then refine with explicit `eq_bands` in the same move.

## Acceptance

Baseline: "Drop the level 3 dB and push it slightly right" places an **EchoJay Gain**
whose `settings_structured.params` is `{"level_db": -3, "pan": 0.25}`, and the client
dials exactly those values — verifiable on the device's readouts, with no anchor map
involved.

Then the depth-pass battery — each should place the right device AND dial the mode:

| Prompt | Expected move |
|---|---|
| "glue the mix bus and stop the kick pumping it" | Compressor `{"mode":"glue","sc_hpf_hz":100,...}` |
| "make the drums punchier" | Compressor `{"mode":"punch",...}` |
| "put it in a plate, quite long" | Reverb `{"algorithm":"plate","decay_s":3.5,...}` |
| "tape echo, eighth notes" | Delay `{"mode":"tape","sync":true,"sync_division":6,...}` |
| "duck the reverb under the vocal" | Reverb `{"duck":40,...}` |
| "warm it up with even harmonics, keep the bass clean" | Saturation `{"emphasis":"even","hpf_hz":120,...}` |
| "give it that old cassette sound" | Tape `{"mode":"cassette",...}` |
| "harmonic tremolo, slow" | Tremolo `{"mode":"bias","rate_hz":3,...}` |
| "narrow the lows, widen the highs" | Stereo Width `{"mode":"multiband","width_low":80,"width_high":140,...}` |
| "turn the sides down 2 dB" | Gain `{"mode":"mid_side","side_db":-2}` |
| "take 3 dB out of the boxiness around 400" | EQ `eq_bands` bell −3 @ 400 |
| "tame the harsh resonances" | EQ `eq_action.tame_resonances` (NO frequencies named) |
| "add air to the sides only" | EQ `eq_bands` high-shelf `channel:"side"` |
| "notch the ring at G5" | EQ `eq_bands` notch `note:"G5"` → 784 Hz |
| "linear phase for the master" | EQ `eq_settings.phase_mode:"linear"` (+ mention latency) |

A full-chain prompt — "make this vocal sit in the mix" — should place several EchoJay
devices and dial each precisely (e.g. EQ → De-Esser → Compressor → Reverb), with modes
chosen sensibly rather than left at default.

## The Key Detector — the suite's first READER (KEY_DETECTOR_SPEC.md)

Every device above is a writer: the model dials, the device processes. The
**EchoJay Key Detector** (category `Analysis`) is the first reader — audio
passes through bit-identically; its output is a `[DETECTED KEY]` block in the
feed. What the backend must teach:

**What the block means.** When present, the client MEASURED the key from live
audio. Two source shapes, distinguished by the block's own header line:

- `[DETECTED KEY — measured by EchoJay on another channel; the KEY OF THE MUSIC]`
  — an EchoJay Link on another channel (ideally the instrumental/mix **bus**)
  detected it. This is the reading to build a vocal chain against: it is the
  key of the music, not of the vocal the chain sits on. It names its source
  channel and carries `age`.
- `[DETECTED KEY — from EchoJay Key Detector in the chain; measured from the
  live signal]` — a Key Detector device in the current chain heard whatever
  flows through THAT chain.

Fields: `key`, `confidence` (0..1), `detected_tuning` (Hz, with cents from
A=440), `root_hz` (the root's fundamental — use it directly, no pitch maths),
`alternates` (runner-up keys with their scores), `analysed`/`age`.

**The confidence rule (hard rule, teach it verbatim).** Below ~0.5, treat the
key as UNKNOWN and do not build moves on it — a confident wrong key is worse
than no key. Close relative-major/minor calls deliberately report low.

**When to use it.**
- "notch the ringing at the root" → send the EQ `note` for the root (or
  `freq_hz: root_hz`) — the block already did the maths.
- "cut the boxiness but keep it off the root" → place the band AWAY from
  `root_hz` and its octaves.
- Musical delay/reverb choices, and interpreting `tame_resonances`: a
  resonance ON the root is the instrument; one between scale degrees is more
  likely a room mode.
- The user asks "what key is this?" → answer from the block (with confidence
  and tuning), NOT from guesswork. If no block is present, say a Key Detector
  (or a Link on the music) is needed — never guess a key.

**Re-measuring.** `analyse` is a dialable param: a move of
`settings_structured.params = {"analyse": 1}` on the Key Detector makes it
listen to the next `window_s` seconds of playback, commit, and hold — the
model can ask the plugin to listen again ("re-check the key after this
modulation"). `reset: 1` clears the held reading.

**Placement guidance to teach.** If the user wants the key of the TRACK while
working on a vocal/bus chain, prefer a Link on the instrumental or mix bus
(its reading arrives automatically); a Key Detector placed in a vocal chain
reads the vocal, which is the worst-case input for key detection and usually
not the question being asked.

Acceptance addition: with a Key Detector in the chain on F# minor material,
"notch the ringing at the root" → an EQ notch at ~92.5 Hz (F#2) or `note:"F#2"`
— the model acting on an observation the plugin made.
