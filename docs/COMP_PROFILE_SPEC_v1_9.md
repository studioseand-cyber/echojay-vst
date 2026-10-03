# EchoJay compressor profiles: spec v1

Status: DRAFT v1.9, 3 Oct 2026. v1.9 (before Kathy's EJ Map run, and her clamp note): `notes` may be one string or a list; a deep point out of rising order or above the ceiling is nulled at load rather than rejecting the profile; and the section 6.4 pick clamp is depth-aware - 12 dB up to a 3 dB ask, then 12 + 2 x (g - 3). v1.8, 2 Oct 2026. v1.8 (Kathy's second review of the deep points): one 0.5 dB hold tolerance for shallow and deep alike, deep_point_error_db is the worst of the SURVIVING deep points with the failures null and listed in `notes`, interpolation runs 1 to 6 (both ways: the level for a GR, and the GR at a level), an all-null position is exported rather than dropped, and a fractional ask whose bound is null borrows that POINT rather than re-reading the whole level. v1.7 (Kathy's review of 6.3, Sean's ruling): the 4/5/6 dB DEEP points - accepted with nulls, the same monotonic rule, their own informational deep_point_error_db that never rejects a profile, the tone check run at each deep level in section 8, and the rule for a pick at a level a position is null at. v1.6 (Sean's rulings after the day's production tests): section 5 - ANY non-null level is accepted, from as little as 3 s heard and from a remembered reading after a restart, and a read under 20 s is labelled on the card; section 6 - four rungs for how compression is set, two GR ceilings (3.0 dB for a build the server chooses, 6.0 dB for an explicit "harder"), the harder/softer ladder, and the sweep scope an explicit ask needs. v1.5 (Kathy's CL 1B export): the section 6 clamp applies to the pick and allows 12 dB. v1.4, 1 Oct 2026: v1.4 (Kathy's third review): detector_f required, point quality is the hold-doubling test, monotonic rule skips nulls and allows equal neighbours. v1.3 (Kathy's second review): measured point quality replaces the model-fit gate, detector_f replaces the detector label, loud_peak_dbfs defined, sweep ceiling -3.01 RMS, in_at_gr_dbfs is the only threshold field read. v1.1: full 64-hex map_fp, measured.reference_ratio, controls_norm. v1.2 (from Kathy's review): measured GR points replace the threshold formula, 997 Hz, RMS convention pinned, stepped controls, detector, tighter acceptance.
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
- `amount.curve`: one point per measured position, at least 9, sorted by norm. For `input_drive` units the position is the input or peak-reduction setting.
- `eff_threshold_dbfs` (v1.3: optional, informational): identical to `in_at_gr_dbfs["1"]` by definition. The server never reads it; `in_at_gr_dbfs` is the only threshold field it uses.
- `amount.curve[].in_at_gr_dbfs` (v1.2, required): for that position, at the reference ratio, the measured input level where GR reaches exactly 1, 2 and 3 dB, read from the raw sweep by interpolation, not from a fitted model. `null` where the sweep never reached it. The sweep tops out at a 0 dBFS peak sine, which is -3.01 dBFS RMS, so no value above -3.01 can exist. This is what the server matches on (section 6), so knee and ratio need no modelling. A fractional g interpolates between neighbouring points, across all six where they are present (v1.8).
- **DEEP POINTS `"4"`, `"5"`, `"6"` (v1.7, optional).** The same measurement at 4, 5 and 6 dB of GR, for the explicit "harder" ladder (6.2). Same rules as the shallow points: a number or `null`, `null` where the sweep never reached it **or where the point failed its hold-doubling test**, nothing above -3.01, and the monotonic rule runs across all of them — within a position, `1 < 2 < 3 < 4 < 5 < 6` where present, nulls skipped. A position described **only** by deep points is still a defect: at least one of 1/2/3 must be there.
- **`quality.point_error_db` is the TRUST GATE and its scope is the 1/2/3 points** (v1.3, unchanged). Over 0.5 dB and the profile is not used at all.
- **`quality.deep_point_error_db` (v1.7, optional) is INFORMATIONAL and never rejects a profile.** It is the same hold-doubling figure for the 4/5/6 points. The server reports it alongside a setting that read a deep point, so a number set from a looser sweep says so.
- **ONE TOLERANCE FOR BOTH (v1.8).** A deep point fails its hold-doubling test at the same **0.5 dB** as a shallow one. A failed deep point is exported as **`null`** and named in `notes`; it is not exported with a loose value and a warning. So `deep_point_error_db` is **the worst figure over the deep points that SURVIVED** — the ones the server actually reads — and on a well-formed export it is therefore 0.5 or less. A larger number means something was exported that should have been nulled, which is worth seeing: the server reads it, reports it, and still never rejects the profile over it.
- **`notes` (v1.8, optional): a list of plain strings, or one string** (v1.9: the exporter writes one string today, and both are accepted). Where a point was nulled for a reason worth recording — a failed hold test, a level the tone check could not confirm, a point out of order — say so here. The server never parses it, and **nothing in `notes` can ever cost a profile**.
- **A DEEP POINT OUT OF RISING ORDER IS NULLED AT LOAD, NOT REJECTED** (v1.9). Where an export carries a 4/5/6 dB point that does not rise above the point below it, or that sits above the sweep ceiling, the server nulls that one point, logs `[comp-profile-deep-point-nulled]` with the position, the point and the reason, and keeps the profile: **a deep point never costs a profile**. A **shallow** point out of order still rejects it — those are the levels every build uses. EJ Map is expected to null these itself in future; the repair covers tonight's export and any already written.
- **AN ALL-NULL POSITION IS EXPORTED, NOT DROPPED (v1.8).** A setting already past 3 dB of GR at the quietest test level has nothing measurable at 1/2/3, and its 4/5/6 are null as well. Export the position with its norm, its display and every point null: that is data, and dropping it would silently shorten the curve and move what the ends mean. The validator accepts it, and the pick and the 12 dB clamp step over it — it is **never filled** from its neighbours, because interpolating a value for it would invent the one thing the export is saying it does not know.
- `amount.stepped` (v1.2): true when the amount control has detents. The curve then lists every detent, and the server only ever picks a listed point, never an interpolated norm.
- `detector_f` (v1.4, REQUIRED, 0..1): how much the unit's detector responds to peaks above RMS. Measured with a two-tone test at the same RMS as the sine: 0 = pure RMS response, 1 = full peak response. Because the two-tone passes through the unit's real attack, f already reflects how much of the peaks it actually reacts to. A profile without it is not used: guessing 0 on a peak-sensitive unit makes the server pick a threshold about 9 dB too low on a typical vocal (RMS -18.4, peak -6.2), so the unit compresses far harder than asked.
- `ratio`: a curve if the ratio is adjustable, else `fixed` with one measured ratio and knee. For program-dependent units (opto), give the ratio measured at 2-4 dB GR.
- `static_gain_db`: output minus input well below threshold, with `neutral` applied.
- `level_coupling` (`input_drive` only): `{ "control": "Input", "gain_db_per_point": [{ "norm": 0.3, "gain_db": -6.0 }, ...] }`: how much the amount control itself changes level below threshold, so the hold can predict OUT.
- `time`: measured when possible, else omit. `program_dependent: true` for opto and auto-release units.
- `fit.max_error_db` (v1.3: informational only, not a gate): worst error against a threshold + ratio + knee model. Soft-knee and opto units legitimately miss that model, so it must not reject them.
- `quality.point_error_db` (v1.4, required): worst disagreement on any `in_at_gr_dbfs` point between the normal sweep and a repeat with the hold doubled (5 s instead of 2.5 s). A point that moves when the hold doubles hadn't settled, which catches slow opto and auto-release units. Identical repeats are not a test, because plugin DSP is deterministic. Systematic errors (stray tones in the reading, makeup coupling, a contaminated reference) are caught by EJ Maps' own checks before export, listed in `notes`. This is the trust gate: over 0.5 dB, the server treats the profile as no profile. The server also rejects a profile whose points are not monotonic: within a position, `in_at_gr_dbfs` must rise from 1 to 2 to 3 dB, and across positions the values must move in one direction. Nulls are skipped, and equal neighbouring values are allowed, so positions that sit past the sweep range don't fail it.

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

### 6.1 Two GR ceilings (v1.6 ruling)

- **3.0 dB** is the most the server asks for **on its own initiative** - the default build. A build asking beyond it is
  held there.
- **6.0 dB** is the most an **explicit ask** may reach. An ask beyond it is held at 6.0.
- The `why` says which ceiling applied, in those words.

### 6.2 Harder and softer on a profiled unit (v1.6 ruling)

A measured unit is a **lookup, not a hunt**: there is no drive loop and no ladder.

- `harder` = the current GR target **+1.5 dB**, up to the 6.0 dB ask ceiling.
- `softer` = **−1.0 dB**, never below **1.0 dB**.
- The new position ships as `controls_norm` with `expected_gr_db`, the ratio held at the profile's reference ratio, in a
  `set` op on that slot. The plugin then runs section 7's check and the make-up a couple of seconds later, which is what
  makes the open-loop write safe.
- **The current target** is read from `gr_target_db` on that slot's own line in `[CURRENT CHAIN]` (the agreed field name).
  Without it the server falls back to the build's own target, so a second "harder" lands on the same number as the first;
  the log line says which source answered.

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

Nothing is extrapolated past what was measured, and nothing is refused for want of one position's point. **This fill is
for the deep levels only**: at 1, 2 and 3 dB the v1.2 rule stands — a null means that position cannot serve that target
and is not guessed at, because those are the levels every build uses.

Where the whole profile stops short of the ask, the server clamps the ask to `measuredMaxGrDb` — **the deepest level any
position carries** — the card reads `"…, as hard as its profile was measured"`, and the block carries
`at_measured_limit: true`.

### 6.4 The computation

Inputs: profile, target GR g (default 2.0 dB; the ceilings in 6.1), and L, the vocal level in the profile's sine-RMS terms:

L = loud_rms + f x (loud_peak - loud_rms - 3.01), where f = `detector_f` (required; no profile is used without it).

At f = 0 this is the RMS level; at f = 1 it is the peak level expressed as an equivalent sine RMS.

1. Ratio: keep the profile's `measured.reference_ratio` (or the unit's fixed ratio), so the measured points apply as they are. Write it through `ratio`. A different requested ratio is a later version; for now the reference ratio wins.
2. For each amount position, read `in_at_gr_dbfs[g]` — **interpolating between the 1 to 6 dB points for a fractional g** (v1.8), so a 3.5 dB ask reads between that position's 3 and 4 dB points. That is the vocal level at which this position gives exactly g. A position with no point at g is handled by 6.3's fill rule; a position with **nothing measured at all** is skipped and never filled.
   The reverse read works over the same range (v1.8): the GR a position gives **at** a level is interpolated across all six of its points, so a level at its 5 dB point reports 5 dB. Past its deepest point the figure is that deepest level, flagged as extrapolated.
3. Pick the position whose value is nearest L, interpolating between positions for a continuous control. For `stepped: true`, take the nearest listed detent.
4. Clamp (v1.5, **depth-aware since v1.9**): apply it to the PICK, not to its neighbours. Interpolate the picked setting's own `in_at_gr_dbfs["1"]` the same way as step 3, and refuse the pick only if that value is more than **the clamp for this ask** below L.
   **The clamp is 12 dB for an ask up to 3 dB, and then 12 + 2 × (g − 3)**: 14 dB at 4, 16 at 5, 18 at 6, interpolated for a fractional ask. 12 dB was set against a 2–3 dB target, and a soft unit needs far more input above its 1 dB point to reach deep GR — the **Lindell SBC measures 14.1 dB** from its 1 dB point to its 6 dB point at every setting, the **CL 1B 15.3** — so a flat 12 dB refused their *measured* 5 and 6 dB picks at any vocal level, which is a clamp overruling a measurement. It still bites: a unit spaced wider than its own clamp is still refused, and the refusal names the depth that applied. Checking the neighbours wrongly excluded valid in-between settings on soft-knee units with sparse positions, and 8 dB capped a very soft unit (CL 1B: 1 and 3 dB points about 10 dB apart) at about 2.3 dB.
5. Send the amount as a 0..1 norm in `controls_norm`; write `engage` + `neutral` + ratio as normal controls; never write `never_touch`.
6. Send `expected_gr_db` (g, or the GR the chosen detent actually gives at L, or — where nothing carried g and the setting's own deepest level answered — that level) and, for `input_drive`, `expected_level_db`. Where the pick read a deep point, `deep_point_error_db` rides with it.

The v1 threshold formula (`T = L - g/(1-1/R) + knee/4`) is withdrawn. It matched a start-of-compression threshold against a 1 dB point, so it always landed about 1 dB too much GR.

## 7. Plugin check (one reading, at most one correction)

- After the dial settles and at least 10 s of loud material has been heard at that slot, measure slot output vs input on the loud phrases. Subtract `static_gain_db` and any expected level change. The result is the observed drop.
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
