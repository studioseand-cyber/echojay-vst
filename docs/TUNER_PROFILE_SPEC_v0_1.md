# EchoJay tuner profiles: spec v0.1 (PROPOSAL)

Status: PROPOSAL v0.1, 4 Oct 2026, from Kathy, for Sean and B to review. Nothing is built against it yet. It follows the same pattern as the compressor contract (COMP_PROFILE_SPEC v2.x): EJ Map measures, the server picks, the plugin dials. If a side needs to change it, change this file first, bump the minor version, and say so.

## 1. Why

People never ask for "85 ms of retune speed". They ask for **more natural**, **a bit more tuned**, **harder** or **the robotic effect**. Every tuner's controls mean something different:

- **Antares Retune Speed** runs from 400 (gentle) to 0 (robotic). Its number isn't even the transition time we measure: at 17 we measured 85 ms, and at 226 we measured 565 ms.
- **bx_crispytuner Amount** runs from 0 to 100, the other way round.
- **Other tuners** use other names, other scales and other directions.

So "more autotune" can't be a fixed knob move. EJ Map measures what each setting actually does to a voice. The server then places every tuner on **one shared feel ladder**, and "more" or "less" is the same-sized step whichever tuner is in the chain. Milliseconds stay inside the system. The user only ever sees feel words.

## 2. Flow

1. EJ Map measures each tuner once, offline, and writes one profile per plugin version (schema below), keyed by `map_fp` like a compressor profile.
2. On a build, or an edit like "more natural" or "harder", the server picks the feel step and computes the settings from the profile (section 6). It sends them as ordinary `settings_structured.controls`, including the key and scale.
3. The plugin dials them. There's no level check for a tuner. Section 7 covers the optional pitch check.
4. **No profile:** today's behaviour, with the card saying "set as dialled, no profile yet".

## 3. The feel ladder

| Step | What the user hears | Primary measure: transition time (starting points, to be set by ear in section 8) | Secondary controls |
| --- | --- | --- | --- |
| 0 Off | No correction | Bypassed, or strength 0 | — |
| 1 Natural | Drifts caught; sounds untouched | About 400 ms or slower | Humanize high, Flex-Tune wide |
| 2 Polished | Clean modern pop | About 150–300 ms | Humanize medium, Flex-Tune medium |
| 3 Noticeable | You can hear it working | About 50–100 ms | Humanize low, Flex-Tune narrow |
| 4 Hard | The robotic, stepped effect | The fastest the unit does (under 21 ms on Auto-Tune) | Humanize off, Flex-Tune off |

- `harder` / "more autotune" moves up one step, to at most 4. `softer` / "more natural" moves down one step, to at least 0. "Hard autotune" or "robotic" goes straight to 4.
- **The default build step is Sean's call.** I'd suggest 2 (Polished) when the user asks for tuning without saying how much, and 0 when they don't ask at all.
- **The card uses feel words only:** "lightly tuned, natural", "tuned, clean pop", "obvious tuning" or "hard autotune effect". Never numbers.
- **The current step is read from `tune_step` on the slot's `[CURRENT CHAIN]` line**, so a second "more" moves on from the first, the same as `gr_target_db` for compressors.

## 4. What EJ Map measures

All measurements use a generated sung-like test note: 220 Hz (A3, in every major scale and in chromatic, so no key write is needed). It runs in a fresh process per position, as for compressors. Stepped controls are measured at **their detents only**, never at in-between norms.

| Property | Signal | What is read | Notes |
| --- | --- | --- | --- |
| **Transition time** (speed) | A note stepping 30 cents off pitch and back (square vibrato) | Duration of each output transition | Already built. **Fix:** the half period adapts, starting at 1 s and stretching up to 4 s, until the output settles (Artist and EFX never settled at 1 s). Faster than one analysis window is recorded as a bound ("faster than 21.4 ms"), never a number. |
| **Strength** | A static note held 30 cents off pitch | Fraction of the detune removed at steady state | Already built. Retune speed doesn't change strength (1.0 at every Auto-Tune position, as expected); Amount-style controls do (crispytuner 0 → 1.07). |
| **Flex-Tune / tolerance** | Static detunes of 5, 10, 20, 30 and 50 cents | The smallest detune that gets corrected | New. On Auto-Tune Pro and Artist, this is what makes "natural" sound natural. |
| **Humanize** | A held note (2 s or more) and short notes (200 ms) with the same 30-cent detune | Correction on the held note against the short note | New. Humanize lets held notes keep movement; 0 means held notes are locked too. |
| **Key and scale write** | — | That a key or scale write lands, and its display text for every key and scale | Writes and read-backs only. The server sets the key from the song (section 6). |
| **Latency** | — | `latency_samples` the plugin reports | Information for the plugin's delay compensation; not part of feel. |

Each property is measured across the control's positions, with every other control held at its instantiate value (recorded in `neutral`), as for compressors.

## 5. Profile schema `ej_tuner_profile/1`

```json
{
  "schema": "ej_tuner_profile/1",
  "plugin": {
    "name": "Auto-Tune Pro", "manufacturer": "Antares", "format": "AudioUnit",
    "plugin_id": "AudioUnit|xxxxxxxx|10.5.0", "version": "10.5.0",
    "map_fp": "<64 hex chars>"
  },
  "measured": {
    "tool": "EJ Maps 0.x", "date": "2026-10-04", "sample_rate": 48000,
    "signal": "220 Hz sung-like note; 30-cent square vibrato; static detunes",
    "latency_samples": 2670
  },
  "speed": {
    "control": "Retune Speed",
    "direction": "lower_is_harder",
    "curve": [
      { "norm": 0.565, "display": "226", "transition_ms": 565, "strength": 1.0 },
      { "norm": 0.0425, "display": "17", "transition_ms": 85, "strength": 1.0 },
      { "norm": 0.015, "display": "6", "transition_ms": 53, "strength": 1.0 },
      { "norm": 0.0, "display": "0", "transition_ms": null, "faster_than_ms": 21.4, "strength": 1.0 }
    ],
    "stepped": false
  },
  "strength": null,
  "flex": {
    "control": "Flex-Tune",
    "curve": [ { "norm": 0.0, "display": "0", "tolerance_cents": 0 }, { "norm": 1.0, "display": "100", "tolerance_cents": 50 } ]
  },
  "humanize": {
    "control": "Humanize",
    "curve": [ { "norm": 0.0, "display": "0", "held_note_correction": 1.0 }, { "norm": 1.0, "display": "100", "held_note_correction": 0.4 } ]
  },
  "key": { "control": "Key", "values": { "C": 0.0, "C#": 0.0909 } },
  "scale": { "control": "Scale", "values": { "Major": 0.0, "Minor": 0.5, "Chromatic": 1.0 } },
  "neutral": [ { "control": "Formant", "set": "Off", "norm": 0.0 } ],
  "never_touch": ["Bypass"],
  "quality": { "transition_spread_ms": 10, "method": "four edges per position; spread = max - min" },
  "notes": []
}
```

The `speed` transition times are Auto-Tune Pro's measured values from Sean's Mac. The norms, and everything under `flex`, `humanize`, `key` and `scale`, are **illustrative only**: they show the shape, not measurements.

### Field rules

- **`speed`:** the control that changes how fast correction happens. `direction` is `lower_is_harder` or `higher_is_harder`, read from the measurement, never assumed from the name. `transition_ms` is null when only a bound was measurable, and `faster_than_ms` then carries the bound.
- **`strength`:** filled only when a separate control changes how far correction goes (crispytuner's Amount is both speed and strength; Auto-Tune's Retune Speed is speed only).
- **`flex` / `humanize`:** optional. Null when the plugin has no such control.
- **`key` / `scale`:** the norm for each key and each scale, from read-backs. Required. A tuner whose key can't be written is not profiled.
- **`quality.transition_spread_ms`:** the worst disagreement between the edges at one position. Proposed gate: a position whose edges disagree by more than 30% of its own transition time is null, and listed in `notes`.
- **The `notes` rule from the compressor spec applies:** a reason for every null.

## 6. Server computation

Inputs: the profile, the feel step (0–4) and the song's key and scale.

1. **Key and scale** come from the song (detected or given), never the plugin's default. This matters more than any other setting: tuning to the wrong key is what makes autotune sound bad.
2. **Speed:** take the step's target band (section 3) and pick the position whose measured `transition_ms` falls in it, the nearest band edge if none falls inside, interpolating for a continuous control and choosing a listed detent for a stepped one. Step 4 takes the fastest position (smallest time or bound).
3. **Secondary controls:** set `flex` and `humanize` to the positions whose measured values match the step's targets (to be fixed in the listening session). Where the plugin lacks them, skip.
4. **Strength:** step 0 sets strength 0 (or bypass, if there's no strength control). Steps 1–4 set strength 1.0, unless the listening session decides "Natural" needs less.
5. **Report:** the card names the step in words. `tune_step` is written to the slot's line.
6. **At the ends:** "harder" at step 4, or "softer" at step 0, says so ("this is already the hardest setting") rather than doing nothing silently.

## 7. Plugin check (optional, later)

A pitch check after dialling, comparable to the compressor's level check: measure the output's pitch steadiness on held notes over the first 10 s, and confirm it fits the step. It's not required for v1, because the profile already describes the behaviour. Worth adding once there's a pitch tracker in the plugin.

## 8. Setting the ladder by ear (one listening session, Kathy and Sean)

The band edges in section 3 are starting guesses. They get fixed by listening, once:

1. **Material:** three short vocals, for example a ballad, a pop lead and a rap hook, each with some natural pitch drift.
2. **Tuners:** Auto-Tune Pro, Auto-Tune Artist and bx_crispytuner (three different control styles).
3. **Renders:** for each tuner, a range of speed settings across the full measured range, rendered offline, with key set correctly and flex and humanize at each candidate step value.
4. **Listen:** blind where possible. Sort each render into Natural / Polished / Noticeable / Hard. The boundaries between groups, in measured transition time, become the band edges.
5. **Consistency check:** the same step on different tuners should land in the same group. If not, the secondary controls need adjusting for that step.
6. **Record the edges** in this spec (v0.2), with the renders kept as reference.

EJ Map can generate the renders: same probe, with a vocal file in place of the test note.

## 9. Acceptance

A tuner profile is accepted when, for each step 1–4, the setting the server would pick (section 6) gives a measured transition time inside that step's band on EJ Map's test note. That's re-measured, not read from the curve, like the compressor tone check. A step that can't be confirmed is null for that plugin, and the server falls to the nearest confirmed step and says so.

## 10. What we have today (Sean's Mac, 3–4 Oct, before the test fixes)

| Tuner | Speed | Strength | State |
| --- | --- | --- | --- |
| Auto-Tune Pro 10.5.0 | 565 ms (226) → 53 ms (6); 0 is faster than 21.4 ms | 1.0 throughout | Good |
| Auto-Tune EFX+ 10.5.0 | Same as Pro | 1.0 throughout | Good |
| bx_crispytuner 1.1.0 | 139–171 ms, then 43–53 ms at Amount 86–100 | 0 → 1.07 | Good: Amount is both speed and strength |
| Auto-Tune Artist 9.5.0 | Not settled within 1 s at most positions | 1.0 throughout | Test fix: adaptive half period |
| Auto-Tune EFX 9.5.0 | As Artist | 1.0 throughout | Test fix: adaptive half period |
| Auto-Tune Access 10.5.0 | Written at in-between positions that don't exist | Measured at 2 of 8 positions | Test fix: detents only (Slow / Medium / Fast) |
| UAD Auto-Tune ×4 | — | — | Held for the UAD-2 Satellite (not an Apollo) |

Flex-Tune, Humanize and key and scale read-backs are not measured yet (section 4, new).

## 11. Questions for Sean

1. **Feel ladder:** are 5 steps (Off to Hard) right, and what's the default step when tuning is asked for without a degree?
2. **Key and scale:** does the server already detect the song's key, or does the plugin send it? Is chromatic the fallback when the key is unknown?
3. **`tune_step`:** OK as the field name on the `[CURRENT CHAIN]` line?
4. **Out of scope for v1:** formant and throat controls, Melodyne-style offline editors (ARA), and harmony and doubling features. Agree?
5. **Listening session:** when suits, with which three vocals?
