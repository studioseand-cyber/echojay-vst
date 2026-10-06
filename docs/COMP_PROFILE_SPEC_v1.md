# EchoJay compressor profiles: spec v1

Status: DRAFT v2.9, 6 Oct 2026. v2.9 (Kathy's spec point and Sean's rulings, 6 Oct): a detector the sweep could not measure is declared in **exactly one form** - `"detector_f": null` with `"detector_f_source": "unknown"` and the reason in `notes` (section 3); the server places on the RMS level (f = 0), the block carries `estimated: true` and `detector_unknown: true`, and the card says the detector is unmeasured (section 6.4); a silently missing `detector_f` is still rejected; and the tone check on such a profile runs at both levels, as if peak and as if RMS, and passes only if both pass (section 8). The first case is the Auto-Tune Vocal Compressor, a plain compressor that passes signal and compresses normally on a licensed run - only the two-tone test fell short of measuring its detector. THIS FILE, at `echojay-saas/docs/COMP_PROFILE_SPEC_v1.md`, is the one official copy from v2.9 on. v2.8, 6 Oct 2026. v2.8 (Sean's ruling, 5 Oct; for A): every profiled calibration block carries **`assumed_in_dbfs`** - the input level the placement was made for - with its metric named in **`assumed_in_metric`**, the **`assumed_in_f`** used and **`assumed_in_source`** (section 6.4 step 6). The plugin measures the same metric at the slot input and logs it beside the server's, which settles level footing. Additive; nothing else changes. v2.7, 4 Oct 2026. v2.7 (agreed with Kathy, 4 Oct; plus Sean's rulings of the same night on the "harder" no-op): **stepped controls** get their own rule (section 3, "STEPPED CONTROLS") - listed detents only, the detent whose measured GR is nearest the target, every detent reaching 1 dB with a minimum of 3 in place of "at least 9", no estimation, the control limit past the last detent, and a stepped output off a listed detent is rejected; **harder and softer step from the TOP of the band**, read `last_gr_db`, and fall back slot line -> the server's own last reply -> the build default (section 6.2); and on the rungs the server sets by its own arithmetic a **working position is read off the map's anchor table** where the turn has it (section 6.0 note). v2.6, 4 Oct 2026. v2.6 (Sean's ruling, 4 Oct): on an UNPROFILED-by-measurement block (rungs 2 and 3) `min_db` / `max_db` now come from the map's anchor table where the map has that control, not from the three-point samples (section 6.4, the v2.6 note); no field is added or renamed. v2.5, 3 Oct 2026. v2.5 (Kathy's ruling after finding the CL 1B's Gain range was built from partial samples): neutral and engage settings go out by NORM only, never by display; a row with no norm is not written at all. v2.4, 3 Oct 2026. v2.4 (Sean's rulings after the 16:10 test): section 7's GR is written as a formula, GR = static_gain_db - (out - in); section 3 states that in_at_gr is referenced to the setting's own low-level gain (static excluded); profiled blocks carry expected_drop_db, the raw in - out the plugin should read; and attaching a profile bumps the map's rev (3.1), because the plugin discards a body whose rev is unchanged. v2.3, 3 Oct 2026. v2.3 (Kathy's design for the live low-level gain reading): every profiled calibration block carries in_at_gr1_dbfs, the dialled setting's own 1 dB point, so the plugin can pick windows that are truly below threshold. v2.2 (Kathy's tidy, no rule changes): section 6.4 step 4 drops the superseded flat-12 rationale that contradicted v2.1, and every stale "1 to 6" / "4/5/6" range reads 1 to 12 and 4 to 12 as it has since v2.0. The code was checked against this and was already right. v2.1 also records Sean's settlement of the 6.3 reading: "never extrapolate from a shallow null" is the HOLE rule (never read across a null), not "no anchor below 3 dB". v2.1 (Sean's ruling on the clamp): for an explicit harder past 3 dB the section 6.4 clamp is the unit's OWN measured spacing from its 1 dB point to g plus a 3 dB margin, not 12 + 2 x (g - 3). A build, and any ask at 3 dB or under, keeps the flat 12. v2.0, 3 Oct 2026. v2.0 (Sean's ruling after the 13:52 test, with Kathy's control-limit and correction notes): an explicit "harder" reaches 12 dB and ESTIMATES above a position's deepest measured point - always flagged; the clamp line continues to 30 dB at 12; deep keys run to 12; a level at which no setting can give the ask reports what the end setting delivers with at_control_limit; and section 7 gains the two-way correction for estimated picks. v1.9, 3 Oct 2026. v1.9 (before Kathy's EJ Map run, and her clamp note): `notes` may be one string or a list; a deep point out of rising order or above the ceiling is nulled at load rather than rejecting the profile; and the section 6.4 pick clamp is depth-aware - 12 dB up to a 3 dB ask, then 12 + 2 x (g - 3). v1.8, 2 Oct 2026. v1.8 (Kathy's second review of the deep points): one 0.5 dB hold tolerance for shallow and deep alike, deep_point_error_db is the worst of the SURVIVING deep points with the failures null and listed in `notes`, interpolation runs 1 to 6 (both ways: the level for a GR, and the GR at a level), an all-null position is exported rather than dropped, and a fractional ask whose bound is null borrows that POINT rather than re-reading the whole level. v1.7 (Kathy's review of 6.3, Sean's ruling): the 4/5/6 dB DEEP points - accepted with nulls, the same monotonic rule, their own informational deep_point_error_db that never rejects a profile, the tone check run at each deep level in section 8, and the rule for a pick at a level a position is null at. v1.6 (Sean's rulings after the day's production tests): section 5 - ANY non-null level is accepted, from as little as 3 s heard and from a remembered reading after a restart, and a read under 20 s is labelled on the card; section 6 - four rungs for how compression is set, two GR ceilings (3.0 dB for a build the server chooses, 6.0 dB for an explicit "harder"), the harder/softer ladder, and the sweep scope an explicit ask needs. v1.5 (Kathy's CL 1B export): the section 6 clamp applies to the pick and allows 12 dB. v1.4, 1 Oct 2026: v1.4 (Kathy's third review): detector_f required, point quality is the hold-doubling test, monotonic rule skips nulls and allows equal neighbours. v1.3 (Kathy's second review): measured point quality replaces the model-fit gate, detector_f replaces the detector label, loud_peak_dbfs defined, sweep ceiling -3.01 RMS, in_at_gr_dbfs is the only threshold field read. v1.1: full 64-hex map_fp, measured.reference_ratio, controls_norm. v1.2 (from Kathy's review): measured GR points replace the threshold formula, 997 Hz, RMS convention pinned, stepped controls, detector, tighter acceptance.
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
    "steps_dbfs": [-63.01, -3.01, 2],
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
      { "norm": 0.00, "display": "-60.0", "in_at_gr_dbfs": { "1": -59.6, "2": -58.3, "3": -57.0 } },
      { "norm": 0.50, "display": "-30.0", "in_at_gr_dbfs": { "1": -29.8, "2": -28.5, "3": -27.2, "4": -26.1, "5": -24.9, "6": null } },
      { "norm": 0.90, "display": "-6.0",  "in_at_gr_dbfs": { "1": -5.8, "2": -4.5, "3": -3.2 } },
      { "norm": 1.00, "display": "0.0",   "in_at_gr_dbfs": { "1": null, "2": null, "3": null } }
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
  "detector_f": 0.0,
  "fit": { "max_error_db": 0.4, "points": 341 },
  "quality": { "point_error_db": 0.2, "deep_point_error_db": 0.6, "method": "hold 2.5 s vs 5 s" },
  "notes": []
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
- `amount.curve`: one point per measured position, sorted by norm. A **continuous** control needs at least 9. A **stepped** control (v2.7) lists **every detent that reaches 1 dB of GR, and needs at least 3** - see STEPPED CONTROLS below. For `input_drive` units the position is the input or peak-reduction setting.
- `eff_threshold_dbfs` (v1.3: optional, informational): identical to `in_at_gr_dbfs["1"]` by definition. The server never reads it; `in_at_gr_dbfs` is the only threshold field it uses.
- `amount.curve[].in_at_gr_dbfs` (v1.2, required): for that position, at the reference ratio, the measured input level where GR reaches exactly 1, 2 and 3 dB, read from the raw sweep by interpolation, not from a fitted model. `null` where the sweep never reached it. The sweep tops out at a 0 dBFS peak sine, which is -3.01 dBFS RMS, so no value above -3.01 can exist. This is what the server matches on (section 6), so knee and ratio need no modelling. A fractional g interpolates between neighbouring points, across all of them from 1 to 12 where they are present (v1.8, extended with the keys in v2.0).
- **NEUTRAL AND ENGAGE GO OUT BY NORM ONLY (v2.5, Kathy's ruling).** A map's numeric range is built from three samples — the displays at parameter 0.000, 0.500 and 1.000 — and an end whose display is a word ("Off", "-Inf") is **dropped**. The CL 1B's `Gain` range therefore reads **8.5 to 31**, the mid-to-top span, while its real taper runs about **−15 ("Off") through 0.0 at norm 0.33 to 31 at 1.0**: a display of `"0.0"` converted through that range lands nowhere near where it was measured. So every `neutral` and `engage` row is sent as its measured **norm** and never as a display, and a row carrying no norm is **not written at all** — the skip says so, because a display that cannot be placed is worse than no write. The **amount** was always norm-only. The **ratio** keeps its label as well as its norm: a labels control is matched by name against its own list, never converted through a numeric taper, so there is no partial range to get wrong.
- **`in_at_gr_dbfs` IS REFERENCED TO THE SETTING'S OWN LOW-LEVEL GAIN** (v2.4, stated because it was never written down): the GR at each point is measured **relative to what that setting does well below threshold**, so the unit's static gain is **excluded**. `static_gain_db` is the separate figure for that gain (out − in below threshold, with `neutral` applied), and the two are never added together on the measurement side. This is how the profiles were made — a quiet reference per position — and it is what makes §7's formula and `expected_drop_db` correct.
- **DEEP POINTS `"4"` … `"12"` (v1.7, extended to 12 in v2.0; optional).** The same measurement at 4, 5 and 6 dB of GR, for the explicit "harder" ladder (6.2). Same rules as the shallow points: a number or `null`, `null` where the sweep never reached it **or where the point failed its hold-doubling test**, nothing above -3.01, and the monotonic rule runs across all of them — within a position, `1 < 2 < 3 < … < 12` where present, nulls skipped. **An existing 1–6 profile stays valid**: every deep key is optional, and a level nobody swept is simply absent. A position described **only** by deep points is still a defect: at least one of 1/2/3 must be there.
- **`quality.point_error_db` is the TRUST GATE and its scope is the 1/2/3 points** (v1.3, unchanged). Over 0.5 dB and the profile is not used at all.
- **`quality.deep_point_error_db` (v1.7, optional) is INFORMATIONAL and never rejects a profile.** It is the same hold-doubling figure for the deep points, 4 to 12. The server reports it alongside a setting that read a deep point, so a number set from a looser sweep says so.
- **ONE TOLERANCE FOR BOTH (v1.8).** A deep point fails its hold-doubling test at the same **0.5 dB** as a shallow one. A failed deep point is exported as **`null`** and named in `notes`; it is not exported with a loose value and a warning. So `deep_point_error_db` is **the worst figure over the deep points that SURVIVED** — the ones the server actually reads — and on a well-formed export it is therefore 0.5 or less. A larger number means something was exported that should have been nulled, which is worth seeing: the server reads it, reports it, and still never rejects the profile over it.
- **`notes` (v1.8, optional): a list of plain strings, or one string** (v1.9: the exporter writes one string today, and both are accepted). Where a point was nulled for a reason worth recording — a failed hold test, a level the tone check could not confirm, a point out of order — say so here. The server never parses it, and **nothing in `notes` can ever cost a profile**.
- **A DEEP POINT OUT OF RISING ORDER IS NULLED AT LOAD, NOT REJECTED** (v1.9). Where an export carries a deep point (4 to 12) that does not rise above the point below it, or that sits above the sweep ceiling, the server nulls that one point, logs `[comp-profile-deep-point-nulled]` with the position, the point and the reason, and keeps the profile: **a deep point never costs a profile**. A **shallow** point out of order still rejects it — those are the levels every build uses. EJ Map is expected to null these itself in future; the repair covers tonight's export and any already written.
- **AN ALL-NULL POSITION IS EXPORTED, NOT DROPPED (v1.8).** A setting already past 3 dB of GR at the quietest test level has nothing measurable at 1/2/3, and its deep points are null as well. Export the position with its norm, its display and every point null: that is data, and dropping it would silently shorten the curve and move what the ends mean. The validator accepts it, and the pick and the 12 dB clamp step over it — it is **never filled** from its neighbours, because interpolating a value for it would invent the one thing the export is saying it does not know.
- `amount.stepped` (v1.2, rewritten in v2.7): true when the amount control has detents. See STEPPED CONTROLS, next.
- **STEPPED CONTROLS (v2.7, agreed with Kathy, 4 Oct 2026).** When a profile's amount control has detents:
  1. **Listed detents only.** The profile marks it `stepped: true` and lists the detent **norms as read back from the plugin**. The server and the plugin only ever write a listed detent norm. A computed target **snaps to the detent whose measured GR is nearest the target** at the track's level (a tie goes to the detent giving less), and the server **never interpolates a position between detents**. The section 6.4 clamp is applied as the detent is chosen - a detent whose own 1 dB point sits more than the allowance under the level is not eligible - because pulling a refused pick back along the curve would land between detents.
  2. **"At least 9 measured positions" is replaced by "every detent that reaches 1 dB of GR, minimum 3".** Fewer than 3 detents reaching 1 dB means the profile is **not publishable**: the exporter refuses it, and the server rejects it at load (`STEPPED_TOO_FEW_DETENTS`).
  3. **Each measured detent gets the normal 0.5 dB hold test**, and `deep_point_error_db` is over the **surviving detents only**.
  4. **No estimation between detents, or past them.** A detent is read only where it was measured: at a level past its deepest measured point it has no figure and is not chosen while any other detent has one. For an ask **past the last detent, `at_control_limit` applies** (section 6.4 step 3 and its message), **not an estimate** - `estimated` never appears on a stepped pick.
  5. **Validation.** A stepped profile whose `controls_norm` output for the amount control is not one of its listed detent norms is **rejected** - the server refuses to send it (`[comp-profile-stepped-off-detent]`) rather than rounding it.
  Example units: UnFairchild (6 detents), Lindell 254E Threshold (16).
- `detector_f` (v1.4, REQUIRED, 0..1): how much the unit's detector responds to peaks above RMS. Measured with a two-tone test at the same RMS as the sine: 0 = pure RMS response, 1 = full peak response. Because the two-tone passes through the unit's real attack, f already reflects how much of the peaks it actually reacts to. A profile without it is not used: guessing 0 on a peak-sensitive unit makes the server pick a threshold about 9 dB too low on a typical vocal (RMS -18.4, peak -6.2), so the unit compresses far harder than asked.
- **A DETECTOR THE SWEEP COULD NOT MEASURE (v2.9, Kathy's spec point, Sean's rulings of 6 Oct 2026).** The v1.4 rule stands for a profile that simply LACKS `detector_f`: it is not used. A profile whose detector the sweep could not measure says so, in **exactly one form**: `"detector_f": null` together with `"detector_f_source": "unknown"`, and the reason in `notes` (a note that names the detector; `notes` may be a list or one string). Nothing else is a declaration: `detector: "unknown"`, `detector_f_source: "assumed"`, `detector_f_assumed`, `detector_unknown` and any other spelling are **rejected**, as is `"unknown"` beside a measured number, and a declaration with no reason in `notes`. A measured profile may say `"detector_f_source": "measured"` beside its number, or say nothing. **First case:** the Auto-Tune Vocal Compressor - a plain compressor, which on Sean's licensed run passes signal and compresses normally; only Kathy's two-tone test fell short of measuring its detector, at a high threshold.
- `ratio`: a curve if the ratio is adjustable, else `fixed` with one measured ratio and knee. For program-dependent units (opto), give the ratio measured at 2-4 dB GR.
- `static_gain_db`: output minus input well below threshold, with `neutral` applied.
- `level_coupling` (`input_drive` only): `{ "control": "Input", "gain_db_per_point": [{ "norm": 0.3, "gain_db": -6.0 }, ...] }`: how much the amount control itself changes level below threshold, so the hold can predict OUT.
- `time`: measured when possible, else omit. `program_dependent: true` for opto and auto-release units.
- `fit.max_error_db` (v1.3: informational only, not a gate): worst error against a threshold + ratio + knee model. Soft-knee and opto units legitimately miss that model, so it must not reject them.
- `quality.point_error_db` (v1.4, required): worst disagreement on any `in_at_gr_dbfs` point between the normal sweep and a repeat with the hold doubled (5 s instead of 2.5 s). A point that moves when the hold doubles hadn't settled, which catches slow opto and auto-release units. Identical repeats are not a test, because plugin DSP is deterministic. Systematic errors (stray tones in the reading, makeup coupling, a contaminated reference) are caught by EJ Maps' own checks before export, listed in `notes`. This is the trust gate: over 0.5 dB, the server treats the profile as no profile. The server also rejects a profile whose points are not monotonic: within a position, `in_at_gr_dbfs` must rise from 1 to 2 to 3 dB, and across positions the values must move in one direction. Nulls are skipped, and equal neighbouring values are allowed, so positions that sit past the sweep range don't fail it.

### 3.1 Serving a profile beside the map (v2.4)

A profile is attached to the map body the plugin loads, at serve time, on **both** paths that return bodies — the maps
endpoint and the inventory lookup. Attaching **bumps the body's `rev`**, and it has to: the plugin skips a body whose `rev`
it already has (`storeParamMaps`: `oldRev == newRev` → skip), and `rev` is stamped at **write** time from the map's own
content, so a profile attached afterwards was invisible to every client that already had the map cached.

- the new `rev` is `sha256(base_rev | "cp:" + profile hash)`, first 12 hex — the same shape as any other rev;
- **`base_rev`** keeps the map's own content rev, so nothing is lost;
- **`comp_profile_rev`** names the 12-hex hash of the profile that was folded in, so the bump is auditable;
- the hash is content-derived, so the same map and profile always give the same `rev`: the body arrives **once**, and a
  re-measured profile propagates because its hash changes.

With the flag off nothing is attached and the `rev` is exactly what was stored.

## 4. Measurement method (recommended)

- Sine 997 Hz (above most sidechain HPFs, inside the vocal band, and off the 1000 Hz default crossover some multiband compressors use). Levels stepped from -60 to 0 dBFS in 2 dB steps.
- Hold each step 2.5 s and read the last 300 ms, so slow releases (LA-2A, opto, auto release) settle. Report levels as sine RMS dBFS.
- Sweep the amount control over at least 9 positions at the reference ratio; on a **stepped** control sweep **every detent** (v2.7) and export each one that reaches 1 dB of GR. Sweep the ratio over its positions at one amount setting.
- Engage check: run one sweep with the engage writes and one without. If both show the same GR, the engage control is wrong; mark `verified: false`.
- Measure at the plugin's default oversampling and quality settings. Note anything unusual in `notes`.
- Licence-bound plugins (UAD, some iLok) fail out of process. Say in `measured.host` how each was measured.

## 5. Level the plugin sends

Per build, on the existing build request, for the track (pre-chain):

```json
"track_level": {
  "loud_rms_dbfs": -18.4,
  "loud_peak_dbfs": -6.2,
  "window": "400ms_p95",
  "heard_s": 90
}
```

- `loud_rms_dbfs`: the 95th percentile of 400 ms RMS over what was heard, as plain RMS: 20*log10(rms of the samples), where a full-scale sine reads -3.01 dBFS. NOT the AES17 convention (full-scale sine = 0 dBFS). EJ Maps exports sine RMS on the same convention (its peak reading minus 3.01). A 3 dB slip here is larger than the whole GR target, so both sides carry a test: a full-scale 997 Hz sine must read -3.01.
- `loud_peak_dbfs` (v1.3): over the same 400 ms windows, the maximum absolute sample value in each window (no oversampling), then the 95th percentile of those across what was heard.
- `heard_s`: how many seconds of signal the reading is made from. It is read, and it is said out loud (below).
- **SEND A READING FROM 3 s (v1.6 ruling, replacing "under 20 s: send `null`").** The plugin sends `track_level` from as
  little as **3 s** of heard signal, and sends a **remembered reading after a restart**. `null` now means only one thing:
  nothing has been heard at all.
- **The server accepts ANY non-null reading.** It never had a duration gate of its own - the 20 s rule was the plugin's -
  and a null level is the only thing that stops the computation.
- **A SHORT READ IS LABELLED.** Where `heard_s` is under **20**, the card says `"set from N seconds of this vocal"`, so the
  number is not read as a settled measurement of the whole take. The seconds **floor**, so 19.9 s reads as "19 seconds"
  and never as the 20 it is below; one second is singular; under a second says "under a second". Section 7's after-check
  is the safety net that corrects a short read once real material has played.

## 6. Server computation (v1.2, amended v1.6)

### 6.0 How compression is set: four rungs (v1.6 ruling)

Every compressor, on a build and on an edit alike, including harder and softer:

| rung | when | what the server does |
| --- | --- | --- |
| 1 | the plugin has a **measured profile** | the profile's **amount control, set directly**: `controls_norm` on the slot, and the calibration block carries `actuator: "amount"`, `param` = that control, `set_directly: true`, `from_profile: true`, `expected_gr_db`, `start_db: null`. **Never the drive actuator.** |
| 2 | **mapped but unprofiled**, and the map has a threshold or amount control | that control **is** the actuator: `param` = its name, with the opening number, `sense` and range |
| 3 | the map has **no usable** threshold or amount control | `actuator: "drive"`, naming nothing - on this rung there is nothing to name |
| 4 | make-up, on every rung | the block names the plugin's **own output/gain control** (`output_param`, with its range; both knobs on a stereo unit) where the map has one. EchoJay's slot output is for the plugins that have none. An "Auto Makeup" switch is not a gain. |

**Where a working position comes from on rungs 2 and 3 (v2.7, Sean's ruling of 4 Oct; for A).** Where the server opens a knob at "a quarter of the way in from the no-compression end" or at mid-travel, that position is now read off the **map's anchor table** - what the dial actually reads at that point of its travel - wherever the turn can fetch the table and it matches the sampled taper, and off the three samples (0.000 / 0.500 / 1.000) otherwise. Which end is no compression is still the sample's to say. The opening values A sees change on the units whose taper is not straight between samples: the **UAD CL 1B and CL 1B mk II `Threshold` open at -2.7** (was -18.5, their half-travel display) and the **CLA-2A `Peak Reduction` at 34.2** (was 21.2). Ruled displays do not move: a dB threshold's -15 and the 1176 family's -15 are rulings, not positions. `[dial-range-source]` says `anchors` or `three_point` for each.

### 6.1 Two GR ceilings (v1.6 ruling)

- **3.0 dB** is the most the server asks for **on its own initiative** - the default build. A build asking beyond it is
  held there.
- **12.0 dB** is the most an **explicit ask** may reach (v2.0, raised from 6.0). An ask beyond it is held at 12.0, so 12.1 is not available.
- The `why` says which ceiling applied, in those words.

### 6.2 Harder and softer on a profiled unit (v1.6 ruling)

A measured unit is a **lookup, not a hunt**: there is no drive loop and no ladder.

- `harder` = the current GR target **+1.5 dB**, up to the **12.0 dB** ask ceiling: 3 → 4.5 → 6 → 7.5 → 9 → 10.5 → 12, and held there. A "harder" already at 12 says it is at the ceiling.
- `softer` = **−1.0 dB**, never below **1.0 dB**.
- The new position ships as `controls_norm` with `expected_gr_db`, the ratio held at the profile's reference ratio, in a
  `set` op on that slot. The plugin then runs section 7's check and the make-up a couple of seconds later, which is what
  makes the open-loop write safe.
- **The current target** is read from `gr_target_db` on that slot's own line in `[CURRENT CHAIN]` (the agreed field name).
  **v2.7 (Sean's rulings, 4 Oct, after a "harder" re-sent the position it was already at):**
  - **The slot line echoes the band exactly as the server last sent it** (lo-hi, or a single value from an older turn), and
    the step is taken from the **TOP** of it. A nudge's band ends on the number asked for, so `[3.0, 3.5]` steps to 5.0.
  - **`last_gr_db`** on the same line is the plugin's last **settled measured** GR, omitted when there is none. The reply
    says both numbers - *"measured about 2.3 dB, now aiming for 5.0"* - and where the measurement sits **more than 1 dB
    above the band's top** the step is taken from the measurement instead. With a measurement and no target anywhere, the
    step is taken from the measurement.
  - **With no target on the line** the order is: the last target the **server itself sent** for that slot, read from the
    history, then the build default. The history a client sends back carries the server's **sentence** and not its block,
    so the sentence is what is read; the plugin's own closing line (*"about 2 dB on the loud phrases, from its profile"*)
    is a rounded measurement and is never read as a target. `[nudge-from-profile]` logs which source answered, with the
    level used and `last_gr_db`.
  - The figure in the reply and on the card is always the block's own target: the top of the band it ships.

### 6.3 THE SWEEP HAS TO COVER WHAT AN ASK MAY REQUEST (v1.6, amended v1.7 after Kathy's review)

Every profile measured under v1.5 carries `in_at_gr_dbfs` points for **1, 2 and 3 dB only**, because this spec capped the
target at 3.0 and nothing was swept past it. With an explicit ask now allowed to 6.0 dB, **the data is the binding
limit**. For an explicit "harder" to reach 4, 5 or 6 dB, the sweeps must carry points at those GR values (§3).

**The two gates are separate** (v1.7). `point_error_db` ≤ 0.5 dB decides whether the profile is used **at all**, and it is
about the 1/2/3 points. The deep points carry `deep_point_error_db`, which is **informational and never rejects the
profile**. A deep point that failed its hold-doubling test arrives as **`null`** — a gap, exactly as at 1/2/3.

**A pick at a level where THIS position is null** (v1.7, Kathy's rule), in order:

1. **Interpolate from the positions that do carry it**, across the norm axis. That is measured data read sideways, so the
   target stands as asked.
   **For a FRACTIONAL g, what is filled is the missing POINT, not the whole level** (v1.8, clarified). A 3.5 dB ask on a
   setting whose 4 dB point is null borrows **that point** from the settings either side of it, and then interpolates
   between this setting's **own** 3 dB measurement and the borrowed 4 dB value. Re-reading 3.5 wholesale from the
   neighbours would discard a real measurement in favour of their shape: on a curve that is not perfectly parallel the two
   answers differ — 0.75 dB of input level on the test curve — and only one of them keeps what this setting actually
   measured. A bound that is SHALLOW and null is never borrowed (the v1.2 rule), so such an ask falls to step 2 instead.
2. **Failing that, the deepest level this position itself carries answers**, and then **the figure reported is that
   level**, not the one asked for. The card must never claim a target the data did not describe.

**This fill is for the deep levels only**: at 1, 2 and 3 dB the v1.2 rule stands — a null means that position cannot serve
that target and is not guessed at, because those are the levels every build uses.

#### "NOTHING IS EXTRAPOLATED" — AMENDED (v2.0)

The rule was absolute and is now bounded, which is a change rather than an addition. **Estimating is allowed, and only
like this:**

1. **ONLY above a position's deepest measured point.** Between measured points everything is interpolation, as before.
   Never across a **hole**: the run stops at the first `null`, so a position whose 3 dB point failed is extended from its
   2 dB point, and a measured 5 dB point sitting above a null 4 is never reached across the gap. A `null` is not a value
   and is never treated as one.
   **This is the confirmed reading** (Sean, 3 Oct 2026): *"never extrapolate from a shallow null"* forbids reading **across**
   a null, not **anchoring below 3 dB**. The stricter reading was considered and rejected — it would make the one-point ratio
   rule unreachable, since a position anchored at 3 dB necessarily has three points.
2. **ONLY for an explicit "harder" past 3 dB.** A build never estimates — it reports what was measured — and neither does
   an explicit ask at or under 3 dB. The server passes this permission in explicitly; it is not a property of the data.
3. **ALWAYS flagged.** `estimated: true` and `estimated_from_gr_db` on the pick and on the calibration block, and the card
   says **"estimated past its measured range"** every single time, exactly as the reverse read already flags an
   extrapolated figure. A number the server estimated must never read like one it measured.

**How:** extend `in_at_gr` from the deepest measured point along **that position's own local slope**, taken from its two
deepest measured points. With only one measured point there is no slope to read and the **measured ratio** stands in: at
ratio R an extra dB of gain reduction needs **R/(R−1) dB** more input.

Everything else about the rule is unchanged: nothing is invented between measured points, nothing is read across a gap,
and no estimate is ever presented as a measurement.

**Superseded in v2.0:** the ask used to be clamped to `measuredMaxGrDb` with the card reading *"as hard as its profile was
measured"*. It now estimates instead (above), and **`at_measured_limit` means the 12 dB ceiling alone**.

### 6.4 The computation

Inputs: profile, target GR g (default 2.0 dB; the ceilings in 6.1), and L, the vocal level in the profile's sine-RMS terms:

L = loud_rms + f x (loud_peak - loud_rms - 3.01), where f = `detector_f` (required; no profile is used without it).

At f = 0 this is the RMS level; at f = 1 it is the peak level expressed as an equivalent sine RMS.

1. Ratio: keep the profile's `measured.reference_ratio` (or the unit's fixed ratio), so the measured points apply as they are. Write it through `ratio`. A different requested ratio is a later version; for now the reference ratio wins.
2. For each amount position, read `in_at_gr_dbfs[g]` — **interpolating between the 1 to 12 dB points for a fractional g** (v1.8, extended with the keys in v2.0), so a 3.5 dB ask reads between that position's 3 and 4 dB points. That is the vocal level at which this position gives exactly g. A position with no point at g is handled by 6.3's fill rule; a position with **nothing measured at all** is skipped and never filled.
   The reverse read works over the same range (v1.8): the GR a position gives **at** a level is interpolated across **all of its points, 1 to 12**, so a level at its 5 dB point reports 5 dB and one between its 7 and 8 dB points reports 7.5. Past its deepest point the figure is that deepest level, flagged as extrapolated.
3. Pick the position whose value is nearest L, interpolating between positions for a continuous control. For `stepped: true` (v2.7) take the listed detent **whose measured GR at L is nearest g** - never an interpolated position, never an estimate - and where the ask is past the last detent, that detent with `at_control_limit`. (Before v2.7 this read "take the nearest listed detent", by level rather than by the GR it gives.)
   **WHERE L IS OUTSIDE THE RANGE IN WHICH ANY SETTING GIVES g** (v2.0, Kathy): the end setting is taken, and
   **`expected_gr_db` is the reverse read of THAT setting at L** — never the ask. The Lindell SBC at −18.4 dBFS gives
   **7.8 dB** from its most aggressive setting, so a 9 dB ask reports 7.8. The block carries **`at_control_limit: true`**,
   a different flag from `at_measured_limit` because the cause is the **control's travel**, not missing data, and
   **estimation never applies here**. The card says: *"about 7.8 dB, the most the Lindell SBC gives at this vocal level. I
   can push the input into it for more if you want."* — the input is **offered, never driven**; driving it is the user's
   decision on a later turn. `gr_target_db` going forward ends on the **delivered** figure (7.8), so a further "harder"
   answers from the limit instead of asking for the impossible again.
4. Clamp (v1.5, the allowance rewritten in **v2.1**): apply it to the PICK, not to its neighbours. Interpolate the picked setting's own `in_at_gr_dbfs["1"]` the same way as step 3, and refuse the pick only if that value is more than **the allowance for this ask** below L. (Checking the neighbours instead wrongly excluded valid in-between settings on soft-knee units with sparse positions, and an 8 dB allowance capped a very soft unit — the CL 1B, whose 1 and 3 dB points sit about 10 dB apart — at about 2.3 dB.)
   **THE ALLOWANCE (v2.1).** For an **explicit harder past 3 dB** it is **the unit's own measured spacing from its 1 dB point to g, plus 3 dB** — taken from the picked position's own points, or from its slope where the figure is estimated. For a **build, and any ask at 3 dB or under**, it stays the **flat 12 dB**.
   **Why it changed.** The v1.9/v2.0 line `12 + 2 × (g − 3)` allowed `(g − 1) × slope ≤ 12 + 2(g − 3)`, which refused the CL 1B (3.06 dB of input per dB of GR) past about **8.5 dB** and a 5-dB-per-dB unit past about **3.6 dB** — at any vocal level. A soft unit's long spacing **is how it compresses**, not a bad pick, so that was a clamp overruling the measurement it exists to protect.
   **What it still catches**, which is the whole of its remaining job: a pick sitting **outside the unit's own measured behaviour by more than the margin**. On a continuous control at an interior setting the two agree by construction, so in practice it bites on a **stepped** control whose nearest detent is more than 3 dB away from where the level actually sits, and on an end setting taken because the level was outside the measured range. The refusal names the allowance it used.
5. Send the amount as a 0..1 norm in `controls_norm`; write `engage` + `neutral` + ratio as normal controls; never write `never_touch`.
6. Send `expected_gr_db` (g; or the GR the chosen detent actually gives at L; or, at a control limit, what the end setting delivers at L) and, for `input_drive`, `expected_level_db`. Where the pick read a deep point, `deep_point_error_db` rides with it. **An estimated pick carries `estimated: true` and `estimated_from_gr_db`; a control-limited one carries `at_control_limit: true`.** The calibration block for either also carries the control's **`sense`** — from the topology, `threshold` is lower-is-harder and `input_drive` higher-is-harder — and the control's **`min_db` / `max_db`**, so section 7's correction can move the right way and never past the end of the dial.
   **`expected_drop_db` (v2.4):** `expected_gr_db − static_gain_db` — the raw `in − out` the plugin should read at the slot, shipped so neither side does sign reasoning at runtime. On a unit with a 0.7 dB static **loss** it is larger than the gain reduction. Absent only where the profile carries no `static_gain_db`, which cannot happen through a valid profile since the field is required.
   **`min_db` / `max_db` COME FROM THE MAP (v2.6, Sean's ruling of 4 Oct; for A).** On a block set from the server's own arithmetic (rungs 2 and 3 of 6.0 - a rung 1 block already read the map), the two bounds are now the ends of the **map's anchor table** for that control wherever the map has it, and the three-point sampled range only where it does not. The values change, the fields do not: the 1176 family's `Input` reads `min_db` **-50.3** (was -24; the MC 77 -50.4), and the CL 1B's `Threshold` reads `max_db` **1.1** (was -18.5). These are the real ends of the dial, which is what section 7's "never past `min_db` / `max_db`" needs. The server's own opening value is bounded separately: an 1176-family or MC 77 `Input` start never goes below **-30** (15 dB under the ruled -15), so `min_db` is how far the dial goes, not how far the server will open it. Each computed write is logged as `[dial-range-source]` with `anchors` or `three_point`.
   **AN UNKNOWN DETECTOR PLACES ON THE RMS LEVEL, AS AN ESTIMATE (v2.9).** Where the profile declares its detector unknown (section 3), L is computed with **f = 0** - the RMS level alone - as the starting estimate. The pick is an ESTIMATE whatever its depth: the block carries **`estimated: true`** and **`detector_unknown: true`** (a build's block and a nudge's alike), so section 7's two-way correction applies; the line and the card say *"estimated: its detector is unmeasured"* and never present the figure as measured; and the assumed level (below) carries `assumed_in_f: 0` and `assumed_in_detector: "unknown"`. The plugin's measure-and-correct step does the rest.
   **`assumed_in_dbfs` (v2.8, Sean's ruling of 5 Oct 2026; for A).** The input level this placement was made for, in the profile's own sine-RMS terms, so the plugin can measure the SAME quantity at the slot input and log the two side by side. It rides every profiled block (a build's and a nudge's) with three companions, all additive:
   - `assumed_in_dbfs`: the number L of this section's first line, e.g. `-16.04`.
   - `assumed_in_metric`: exactly `detector_weighted_level_dbfs = loud_rms_dbfs + f * (loud_peak_dbfs - loud_rms_dbfs - 3.01); loud_rms_dbfs and loud_peak_dbfs as spec section 5 (400 ms window, p95)` - the formula the plugin computes at the slot input, from ITS loud-phrase RMS and peak over the same window.
   - `assumed_in_f`: the `f` used, the profile's `detector_f` (0.43 on the Tube-Tech CL 1B); `0` when the turn carried no peak reading and the RMS was used alone.
   - `assumed_in_source`: where `loud_rms_dbfs` / `loud_peak_dbfs` came from: `"track_level"` (the track analysis the request carried) until a per-slot reading exists, then `"slot_level"`.
   Worked, from the 5 Oct session: rms -21.45, peak -5.85, f 0.43 -> L = -21.45 + 0.43 x (15.60 - 3.01) = **-16.04**. A plugin reading at the slot input that lands 2 to 5 dB under that figure says the EQ and de-esser ahead of the compressor took it; one that lands on it says the level footing is not the cause of the 0.7 dB shortfall.
   **`in_at_gr1_dbfs` (v2.3):** every profiled block carries **the dialled setting's own 1 dB point**, interpolated at the chosen norm exactly as the pick reads it — and recomputed if the clamp pulled the pick back, so it describes the setting that actually shipped. The plugin uses it to choose windows that are **truly below threshold** for the low-level gain reading (§7). A block with no profile behind it omits the field: there is no measurement to report.

The v1 threshold formula (`T = L - g/(1-1/R) + knee/4`) is withdrawn. It matched a start-of-compression threshold against a 1 dB point, so it always landed about 1 dB too much GR.

## 7. Plugin check (one reading, at most one correction)

### 7.0 The two-way correction, for ESTIMATED picks only (v2.0, Kathy)

A measured pick keeps the rule below exactly as it is: **one correction, toward less compression only.** An **estimated**
pick — `estimated: true` — is a number the server extended past the measurements, so it may be wrong in either direction,
and the correction is two-way:

- **Applies ONLY to a pick with `estimated: true`.**
- **Trigger:** the level-minus-`static_gain_db` gain reduction misses `expected_gr_db` by **more than 1 dB, either
  direction**. **Never on the crest reading** — that is not a GR measurement.
- **At most ONE move.** The **named control** (`param`) moves in the block's **`sense`** by the amount the position's own
  slope says closes the gap, **never past `min_db` / `max_db`**, and **never EchoJay's drive**.
- **After it, hold and report the measured figure.** If it is still off, the card says so and **asks**; there is no second
  move.
- **An `at_control_limit` pick gets no correction at all** — there is no travel left to correct with.

- After the dial settles and at least 10 s of loud material has been heard at that slot, measure slot output and input on the loud phrases. The gain reduction is
  **`GR = static_gain_db − (out − in)`** (Kathy's formula, written out in v2.4 — the word "subtract" was ambiguous and its sign was read both ways, which cost a live test). Subtract any expected level change as well. The result is the observed drop.
  Worked: a unit whose `static_gain_db` is **−0.7** showing a 2.7 dB level drop on the loud phrases is doing `−0.7 − (−2.7) = 2.0 dB` of gain reduction. The server ships **`expected_drop_db`** (§6.4 step 6) so the raw `in − out` figure can be compared directly, without deriving it.
- Observed drop more than expected + 3 dB: move the amount control once by the difference (toward less GR), log it, no second move.
- Observed drop under 0.5 dB when expected is 1 dB or more: log `PROFILE_NOT_ENGAGING` with plugin, map_fp and the readings. Change nothing. This is how a wrong profile gets reported.
- Then the hold matches level on OUT once, as now.
- Closing line per compressor, for example: "EMO-D5: about 2 dB on the loud phrases, from its profile. Output -1.5 dB." or "NEOLD U2A: set as dialled, no profile yet."

## 8. Acceptance

A profile is accepted when, on the reference vocal clip (`refs/vocal_ref_01.wav`, to be chosen by Sean), an offline render with EchoJay's computed settings gives a measured GR on loud phrases within 1.0 dB of the target, AND a 997 Hz tone at L through the same settings lands within 0.5 dB of g. The tone check catches calculation errors the 1 dB vocal tolerance would hide. Licence-bound plugins are accepted on Kathy's Mac, where the iLok is.

**THE TONE CHECK RUNS AT EVERY DEEP LEVEL THE PROFILE CARRIES** (v1.7). For each of 4, 5 and 6 dB that the profile has
points for, run the same 997 Hz check at that g, to the same **0.5 dB** tolerance. **A level that fails is `null` across
all positions** — the whole level comes out, not the one position that missed, because a level the tone check cannot
confirm is a level the server must not pick from. Failing a deep level does not fail the profile: the shallow points and
any deep level that passed are unaffected, which is the whole point of separating the gates (6.3).

**WITH AN UNKNOWN DETECTOR THE TONE CHECK RUNS AT BOTH LEVELS** (v2.9, for the contract). Where the detector could not be
measured (section 3), each check runs twice: once at the level the point implies **as if the detector were RMS** (f = 0)
and once **as if it were peak** (f = 1), and the point passes only if **both** pass to the same 0.5 dB tolerance. A point
that passes under one reading and not the other is `null`, named in `notes`.

**AND THE HOLD TEST AT THE SAME 0.5 dB** (v1.8). A deep point whose value moves by more than 0.5 dB when the hold is
doubled is **`null`**, named in `notes`, exactly as a shallow one would be rejected. What survives is what the server
reads, so `quality.deep_point_error_db` is the worst figure **over the survivors** (§3).

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
