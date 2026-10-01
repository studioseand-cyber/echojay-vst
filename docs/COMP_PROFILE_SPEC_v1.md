# EchoJay compressor profiles: spec v1

Status: DRAFT v1.2, 1 Oct 2026. v1.1: full 64-hex map_fp, measured.reference_ratio, controls_norm. v1.2 (from Kathy's review): measured GR points replace the threshold formula, 997 Hz, RMS convention pinned, stepped controls, detector, tighter acceptance.
This file is the contract between the three. If a side needs to change it, change this file first, bump the minor version, and say so.

## 1. Why

Live tests on 30 Sep showed that finding 2-3 dB of gain reduction by driving a third-party compressor and reading level in vs level out does not work. The reading is fooled by engage switches left off (EMO-D5 COMP, NEOLD V76 bypass, VComp), saturation stages (NEOLD U2A Drive), mix knobs (NEOLD Mix 80), auto makeup, and fixed-threshold designs (1176, LA-2A). Each fix covered one plugin and the next plugin broke it.

New approach: measure each compressor once, offline, with test tones (EJ Maps). Store the measurement as a profile. At build time, set the compressor open-loop from the track's measured level and the profile. Check once. Match level once. No drive seek.

## 2. Flow

1. EJ Maps sweeps a plugin and writes one profile JSON (schema below) per plugin version.
2. The profile is published next to that plugin's parameter map on the server, keyed by the same map fingerprint (`map_fp`). Until a publish path exists, profiles are JSON files in `comp_profiles/` in echojay-saas, imported at deploy.
3. On a build, the server receives the track's loud-phrase level from the plugin (section 5), computes the compressor settings from the profile (section 6), and sends them as ordinary `settings_structured.controls`, including the engage writes.
4. The plugin dials them as it dials any map, takes ONE check reading (section 7), corrects at most once, then matches level on OUT once.
5. No profile for a plugin: today's behaviour (letter (q)): set as dialled, level matched, and the line says "set as dialled, no profile yet".

## 3. Profile schema `ej_comp_profile/1`

```json
{
  "schema": "ej_comp_profile/1",
  "plugin": {
    "name": "EMO-D5 (s)",
    "manufacturer": "Waves",
    "format": "AudioUnit",
    "plugin_id": "AudioUnit|xxxxxxxx|15.0.70",
    "version": "15.0.70",
    "map_fp": "<64 hex chars, full fingerprint>"
  },
  "measured": {
    "tool": "EJ Maps 0.x",
    "date": "2026-10-01",
    "sample_rate": 48000,
    "signal": "sine 997 Hz, stepped",
    "level_ref": "sine_rms_dbfs",
    "steps_dbfs": [-60, 0, 2],
    "hold_ms": 2500,
    "read_window_ms": 300,
    "reference_ratio": 4.0,
    "host": "out_of_process | in_host"
  },
  "topology": "threshold",
  "engage": [
    { "control": "Comp", "set": "On", "norm": 1.0, "verified": true }
  ],
  "never_touch": ["Power", "Bypass"],
  "neutral": [
    { "control": "Comp Mix", "set": "100", "norm": 1.0 },
    { "control": "Makeup", "set": "0.0", "norm": 0.5 },
    { "control": "Auto Makeup", "set": "Off", "norm": 0.0 }
  ],
  "amount": {
    "control": "Comp Thresh",
    "curve": [
      { "norm": 0.00, "display": "-60.0", "eff_threshold_dbfs": -59.6, "in_at_gr_dbfs": { "1": -59.6, "2": -58.3, "3": -57.0 } },
      { "norm": 0.50, "display": "-30.0", "eff_threshold_dbfs": -29.8, "in_at_gr_dbfs": { "1": -29.8, "2": -28.5, "3": -27.2 } },
      { "norm": 1.00, "display": "0.0",   "eff_threshold_dbfs": 0.2,   "in_at_gr_dbfs": { "1": 0.2, "2": 1.5, "3": null } }
    ],
    "stepped": false
  },
  "ratio": {
    "control": "Comp Ratio",
    "curve": [
      { "norm": 0.10, "display": "2.00", "measured_ratio": 1.9, "knee_db": 6.0 },
      { "norm": 0.20, "display": "4.00", "measured_ratio": 3.8, "knee_db": 6.0 }
    ],
    "fixed": null
  },
  "static_gain_db": 0.0,
  "level_coupling": null,
  "time": { "attack_ms": 9.8, "release_ms": 80.0, "program_dependent": false },
  "detector": "rms",
  "fit": { "max_error_db": 0.4, "points": 341 },
  "notes": ""
}
```

### Field rules

- `plugin.map_fp` is the join key: the FULL 64-hex fingerprint EchoJay computes for that plugin's parameter map. The `fp=` in EJDialSummary logs is only its first 12 characters and will not match. If EJ Maps cannot compute the full value, give `plugin_id` + `version` and the server resolves it.
- `measured.reference_ratio`: the ratio the amount curve was measured at. Required whenever the ratio is adjustable.
- `control` names are the plugin's own parameter names, exactly as in its parameter map. `norm` is the 0..1 value written. `set` is the display text it reads back as.
- `topology`:
  - `threshold`: a dB threshold control (EMO-D5, Pro-C 2, Tube-Tech CL 1B, Mike-E).
  - `input_drive`: fixed threshold, amount set by input or peak reduction (1176 family, LA-2A family, NEOLD U2A). `level_coupling` is then required (below).
  - `other`: anything else. The server does not auto-set it; it is treated as no profile.
- `engage`: every write needed for the unit to compress. `verified: true` means the sweep showed GR with these writes and none without them. Engage must never write a control named in `never_touch`.
- `never_touch`: controls EchoJay must not write (power, standby, plugin bypass). It must still list them, so the consumer knows they exist.
- `neutral`: the settings the sweep was measured at. The server writes them too, so the measurement holds: mix 100% wet, makeup 0 or off, drive or saturation at its cleanest, sidechain filter off where possible.
- `amount.curve`: one point per measured position, at least 9, sorted by norm. `eff_threshold_dbfs` is the input level (in `measured.level_ref` units) where gain reduction reaches 1.0 dB with ratio at the profile's reference ratio. For `input_drive`, it is the input level at which GR reaches 1.0 dB with the amount control at that position.
- `amount.curve[].in_at_gr_dbfs` (v1.2, required): for that position, at the reference ratio, the measured input level where GR reaches exactly 1, 2 and 3 dB, read from the raw sweep by interpolation, not from a fitted model. `null` where the sweep never reached it. This is what the server matches on (section 6), so knee and ratio need no modelling.
- `amount.stepped` (v1.2): true when the amount control has detents. The curve then lists every detent, and the server only ever picks a listed point, never an interpolated norm.
- `detector` (v1.2, optional): `rms`, `peak` or `unknown`, if EJ Maps can tell (for example by comparing a sine with a high-crest test signal at equal RMS). A peak detector reacts to vocal peaks that a sine doesn't have, so the server uses `loud_peak_dbfs - 3.01` instead of `loud_rms_dbfs` for it.
- `ratio`: a curve if the ratio is adjustable, else `fixed` with one measured ratio and knee. For program-dependent units (opto), give the ratio measured at 2-4 dB GR.
- `static_gain_db`: output minus input well below threshold, with `neutral` applied.
- `level_coupling` (`input_drive` only): `{ "control": "Input", "gain_db_per_point": [{ "norm": 0.3, "gain_db": -6.0 }, ...] }`: how much the amount control itself changes level below threshold, so the hold can predict OUT.
- `time`: measured when possible, else omit. `program_dependent: true` for opto and auto-release units.
- `fit.max_error_db`: worst error between the model (threshold + ratio + knee) and the measured points. Over 1.5 dB means the profile is not trusted and the server treats it as no profile.

## 4. Measurement method (recommended)

- Sine 997 Hz (above most sidechain HPFs, inside the vocal band, and off the 1000 Hz default crossover some multiband compressors use). Levels stepped from -60 to 0 dBFS in 2 dB steps.
- Hold each step 2.5 s and read the last 300 ms, so slow releases (LA-2A, opto, auto release) settle. Report levels as sine RMS dBFS.
- Sweep the amount control over at least 9 positions at the reference ratio. Sweep the ratio over its positions at one amount setting.
- Engage check: run one sweep with the engage writes and one without. If both show the same GR, the engage control is wrong; mark `verified: false`.
- Measure at the plugin's default oversampling and quality settings. Note anything unusual in `notes`.
- Licence-bound plugins (UAD, some iLok) fail out of process. Say in `measured.host` how each was measured.

## 5. Level the plugin sends

Per build, on the existing build request, for the track (pre-chain):

```json
"track_level": {
  "loud_rms_dbfs": -18.4,
  "loud_peak_dbfs": -6.2,
  "window": "400ms_rms_p95",
  "heard_s": 90
}
```

- `loud_rms_dbfs`: the 95th percentile of 400 ms RMS over what was heard, as plain RMS: 20*log10(rms of the samples), where a full-scale sine reads -3.01 dBFS. NOT the AES17 convention (full-scale sine = 0 dBFS). EJ Maps exports sine RMS on the same convention (its peak reading minus 3.01). A 3 dB slip here is larger than the whole GR target, so both sides carry a test: a full-scale 997 Hz sine must read -3.01.
- Under 20 s heard: send `null`. The server then does not compute a threshold and falls back to today's behaviour.

## 6. Server computation (v1.2)

Inputs: profile, L = `track_level.loud_rms_dbfs` (or `loud_peak_dbfs - 3.01` when `detector` is `peak`), target GR g (default 2.0 dB; never above 3.0).

1. Ratio: keep the profile's `measured.reference_ratio` (or the unit's fixed ratio), so the measured points apply as they are. Write it through `ratio`. A different requested ratio is a later version; for now the reference ratio wins.
2. For each amount position, read `in_at_gr_dbfs[g]` (interpolating between the 1, 2 and 3 dB points for a fractional g). That is the vocal level at which this position gives exactly g.
3. Pick the position whose value is nearest L, interpolating between positions for a continuous control. For `stepped: true`, take the nearest listed detent.
4. Clamp: never pick a position whose `in_at_gr_dbfs["1"]` is more than 8 dB below L.
5. Send the amount as a 0..1 norm in `controls_norm`; write `engage` + `neutral` + ratio as normal controls; never write `never_touch`.
6. Send `expected_gr_db` (g, or the GR the chosen detent actually gives at L) and, for `input_drive`, `expected_level_db`.

The v1 threshold formula (`T = L - g/(1-1/R) + knee/4`) is withdrawn. It matched a start-of-compression threshold against a 1 dB point, so it always landed about 1 dB too much GR.

## 7. Plugin check (one reading, at most one correction)

- After the dial settles and at least 10 s of loud material has been heard at that slot, measure slot output vs input on the loud phrases. Subtract `static_gain_db` and any expected level change. The result is the observed drop.
- Observed drop more than expected + 3 dB: move the amount control once by the difference (toward less GR), log it, no second move.
- Observed drop under 0.5 dB when expected is 1 dB or more: log `PROFILE_NOT_ENGAGING` with plugin, map_fp and the readings. Change nothing. This is how a wrong profile gets reported.
- Then the hold matches level on OUT once, as now.
- Closing line per compressor, for example: "EMO-D5: about 2 dB on the loud phrases, from its profile. Output -1.5 dB." or "NEOLD U2A: set as dialled, no profile yet."

## 8. Acceptance

A profile is accepted when, on the reference vocal clip (`refs/vocal_ref_01.wav`, to be chosen by Sean), an offline render with EchoJay's computed settings gives a measured GR on loud phrases within 1.0 dB of the target, AND a 997 Hz tone at L through the same settings lands within 0.5 dB of g. The tone check catches calculation errors the 1 dB vocal tolerance would hide. Licence-bound plugins are accepted on Kathy's Mac, where the iLok is.

## 9. First plugins (Sean to confirm)

EMO-D5 (s), NEOLD U2A, NEOLD V76U73, Tube-Tech CL 1B, Empirical Labs Mike-E Comp, an 1176 (CLA-76 or UAD 1176), an LA-2A (CLA-2A or UAD LA-2A), VComp, Pro-C 2, and one more to replace Logic Compressor (Logic's own plugins can't load outside Logic, so they can't be swept).

## 10. Open questions for Kathy

1. What does EJ Maps output today? Share one real file.
2. Can it compute EchoJay's `map_fp`, or should it give plugin id + version?
3. Which plugins can it load, and how does it handle licence-bound ones?
4. Can it find and verify engage switches, or do those need hand entry?
5. How does it publish maps now, and can profiles ride the same path?

## 11. Version matching (v1.2)

Profiles key to the plugin version they were swept on. Waves on the EJ Maps Mac is V12; EchoJay test machines run 15.0.70. A V12 profile will not match a 15.x map. Sweep on the version users run.
