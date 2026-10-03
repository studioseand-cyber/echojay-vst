# Reply to COMP_PROFILE_SPEC_v1.md, section 10

1 Oct 2026, from the EJ Map certification work on `feat/ejmap-cert`. Every number here is
measured on this Mac (M1 Pro, arm64, JUCE 8.0.12 AU hosting) and can be re-derived from the
committed traces under `tools/ejmap/cert-traces/`. Where something is NOT measured it says so.

## 1. What EJ Maps outputs today

One JSON fixture per plugin identity (`AudioUnit_<uid>_<version>.json`), in
`tools/ejmap/cert-fixtures/profiles/` (98 today). The real one beside this file:

**`docs/COMP_PROFILE_REPLY.fixture.json` = Tube-Tech CL 1B 2.5.62**, one of your first ten.

What it carries, against your schema:

| your field | ours | note |
|---|---|---|
| `plugin.name/format/version` | `product`, `format`, `version`, `uid` | `identity` object too |
| `plugin.map_fp` | NOT in the fixture today | see 2 |
| `measured.*` | `thresholdSweep.measuredAt`, `host` ("EJ Map 0.1.0 / arm64 / 48k"), `bridged`, `tone` {hz 997, levels −24/−12/−6 dBFS **peak**}, `procedure` | hold 1.5 s, discard 0.75 s, read window 0.25 s; **peak**, not sine RMS (ours is 3.01 dB above yours for a sine) |
| `topology` | `thresholdReview.class` on multi-threshold fixtures; single-threshold fixtures have no field | `bands_or_stages`, `input_as_threshold`, `channels_lr` |
| `engage[]` | NOT found by us | see 4 |
| `never_touch[]` | not emitted | roles exist per control (`controls[].role`); bypass/power are not roled, so not listed either |
| `neutral[]` | `thresholdSweep.ratioDuring` (ratio raised to the smallest read ≥ 4:1, value read back), `autoMakeupDisabled` | everything else at instantiate defaults; `defaults.json` sidecar holds every control's instantiate value and display text |
| `amount.curve` | `positionNorms[16]` + `texts` (display) + `reduction_db{-24,-12,-6}[16]` + `thresholdDbEquivalent[16]` | per position: norm, display, GR at three levels, and T = L − g·R/(R−1) where readable; `{above:-6}` / `{below:-24}` outside the readable band |
| `ratio` | `ratioDuring` only (one ratio, read back) | no ratio curve, no knee |
| `static_gain_db` | `defaultGain_db{-24,-12,-6}` at the default threshold | recorded, never judged |
| `level_coupling` | input-as-threshold fixtures use a per-position quiet reference (`linearReference.mode: per_position_quiet`, gain at −48 checked against −54) | the per-position linear gain IS your gain_db_per_point |
| `time` | not measured | |
| `fit.max_error_db` | no model fit; instead `levelDependence` (dg/dL per position vs 1−1/R), `displayEngage`, `displayOffsetDb` | see feedback below |
| `verified`-style flags | `result` ∈ certified / flat / nonmonotonic / unreadable, with `reason`; `passThroughAtDefaults`; `notToneReadings`; `linearReference.{error_db, response_db, error_fraction}` | every refusal names what it saw |

CL 1B's numbers: 16 positions, ratio ran at 6:1 (read back), lower-is-harder, engages at
position 3 at all three levels, T map −14.2 / −18.7 / −24.5 dBFS-peak at positions 3–5, the
three hardest positions `above −6` (saturated at the loudest test level). Default gain −0.64 dB.

## 2. `map_fp`: yes, with one caveat — checked, not asserted

EJ Map's mapper and EchoJay share one header (`EchoJayParamMaps.h`,
`fingerprintForDescription`): `SHA-256("format|uidHex|version|param_count")`, `param_count`
from `getParameters().size()` on a loaded instance. The certification probe loads the same way.

**Check:** for the 92 store fixtures that also have a local EJ Map map, I recomputed the hash
from the fixture's `format|uid|version|len(controls)` and compared it with the map's `fp`:
**87 of 92 match**. The 5 that do not (Auto-Tune Vocal Compressor 59 vs 71, DynOne3 113 vs 114,
NEOLD U2A 11 vs 13, NEOLD U17 16 vs 18, TBTECH Cenozoix 98 vs 99) all match when the map's own
`param_count` is used instead — so the hash is right and `len(controls)` is wrong on those five:
the probe's raw `--list-params` prints the full count (NEOLD U2A: 13 rows), and the fixture's
`controls` array has fewer after composition. Cause not examined (logged as a lead).

**Changed 1 Oct (the field is new — read it, do not compute it):** every record now carries
`param_count` (the probe's raw `--list-params` row count = `getParameters().size()`) and
`map_fp` computed from it by the shared `fingerprintForDescription`. Backfilled on all 103
records from their traces; **96 of 96 records that have a local EJ Map map reproduce that map's
`fp`**, the five former misses included, and the suite pins that against the corpus (M1). Cause of
the misses: `controls` is built from the text-at rows, and a parameter the text pass skipped was
never a control. Join on `map_fp`; `plugin_id` + `version` remains the fallback when a record
predates this field.

## 3. Which plugins we can load, and licence-bound ones

Census on this Mac, 1 Oct, read-only (`--cert-sweep-census`, PACE included):

- **98 fixtures recorded** (33 single-threshold certified, 42 multi-threshold candidates
  fixtures, 15 flat, 7 unreadable, 1 nonmonotonic).
- **62 more compressors discovered** (installed, mapped, no fixture): **36 UAD-2 need UAD
  hardware** (all `!UAD`, none UADx native) and **4 McDSP APB need the APB box** — recorded as
  CONDITIONAL, one re-run once attached; **22 runnable** and queued.
- **Licence-bound (PACE/iLok):** 6 of the runnable 22 are PACE-wrapped; the earlier batch-4
  census counted 27 licence-bound across the then-118 discovered — that number is from 29 Sep
  and is not re-measured here. With the iLok in, PACE products load and certify in the
  out-of-process probe (SSL Native ×3, U73b, kHs, CL 1B, SSL G3, Zip…); without it they show
  PACE's activation window and are refused `transient`, re-run with `--retry-refused`.
- `measured.host`: always **out of process**, in a signed helper (`EchoJayProbe`), one process
  per position, 48 kHz, enable-all-buses with sidechains fed silence.

**The blocker you need to know about:** PACE refuses the helper unless it carries a real
Developer ID Team Identifier. The probe is signed here; **`ejmap.app` itself is not signed or
notarised anywhere** (RELEASE.md signs the two plugins). So on any mapper's Mac but this one,
no PACE-wrapped compressor can be certified until a signed, notarised ejmap.app carrying the
probe exists. Native, licence-free products run anywhere.

Architecture: x86_64-only bundles (most Waves) run bridged under AUHostingServiceXPC and
certify fine; `bridged: true` is recorded.

## 4. Engage switches: we cannot find them yet

Plainly: **no.** The sweep writes the threshold candidate and reads GR; it does not search for
what makes the unit compress. Evidence, from the store, 1 Oct: **6 products are
`pass_through_at_defaults` on every candidate** — output equals input (plus a constant) at
every position and level, 34 candidates in all:

| product | candidates | constant |
|---|---|---|
| DynOne3 | 15 | 0.00 dB |
| EMO-D5 (m) / (s) | 5 + 5 | 0.00 |
| MaxxVolume (m) / (s) | 4 + 4 | 0.00 (its two "…Thresh On" switches, swept, change nothing) |
| dbx-160 (s) | 2 | +0.31 dB |

Plus single-threshold members of the same family: C1 comp (m/s), RCompressor (m/s), OneKnob
Pressure (m/s), Low Control, **SSLComp (m/s) at +3.00 dB**, dbx-160 (m) +0.31. Your section 1
is right that this is the failure that keeps coming back. Item 3 of today's plan builds the
two-sweep engage test (with candidate writes / without; GR only with them = `verified`); until
it exists, engage is hand entry and this reply says so.

## 5a. One store, two record kinds — nothing in your contract changes (ruled 1 Oct)

Compressor and tuner certification records live in ONE directory and ride one zip; each record
carries a `schema` field (`ej_cert_compressor/1`, `ej_cert_tuner/1`) and that field alone tells
them apart. Your `ej_comp_profile/1` is the server-side projection built from the compressor
records and is untouched by this; the tuner records are a second projection for section 5 of
your spec when you want it. You never see a mixed file: a record is one kind.

## 5. Publishing: yes, the same path

Since 30 Sep the sweep writes to `~/Library/ejmap/cert/` by default (`fixtures/` is the store,
traces and reports beside it), so profiles ride the runbook's `zip -rq ~/Library/ejmap`
hand-over and the operator's ingest exactly as maps do. No new transport. Keyed today by
identity (`AudioUnit|uid|version`); `map_fp` joins once item 2's change lands.

## Measured feedback on the spec

- **997 Hz, not 1000 Hz.** MDynamicsMB and MDynamicsMBLarge instantiate `Crossover → Cross 2 =
  1000 Hz` exactly (200 / 1000 / 4634). A 1 kHz tone lands ON the band 2/3 crossover; at 997 Hz
  both bands already respond with near-identical curves. Any multiband that defaults a crossover
  to 1 kHz will do this. Keep 997 (or any non-round value), and expect two adjacent bands to
  answer when a crossover sits there — read `responding` against the crossover defaults.
- **`fit.max_error_db` 1.5 dB** is 75 % of your own 2 dB GR target, and it is an absolute bar on
  a relative quantity. We had the same shape (2.0 dB soft-end spread, 0.5 dB above-reference)
  and it lost three real curves by 0.10–0.67 dB while passing a 1.2 dB error on a 10 dB response
  as "noise". The replacement is **error as a fraction of the largest measured response**
  (ours: 1/12, the sense resolution over the readable band), recorded as a number, not fitted.
  The distribution over 83 references was bimodal — 48 at ≤ 2 %, a band at 8–19 % that is every
  multiband — and that split is what told us the cause (other stages compressing at default),
  which an absolute bar would have hidden.
- **`static_gain_db` "well below threshold" is unsafe.** OTT compresses *upward* at −54 dBFS:
  its −48 minus −54 reads −4.5 dB, not −6. Measure two quiet levels 6 dB apart and require the
  outputs to differ by 6 dB within 0.1 before calling either "linear"; refuse the position
  otherwise. That self-check is built (`linearReference.check_db`), and it is the only thing
  that told us OTT has no linear level at any setting.
- **Topology "other" is a large bucket, not an edge case.** Of 98 fixtures: **42 are
  multi-threshold** (39 bands-or-stages, 2 input-as-threshold pairs, 1 L/R pair — including two
  spectral dynamics and two always-on units), **14 are input-as-threshold** (your
  `input_drive`), and the rest single dB-threshold. By your definition that is 42 of 98 (43 %)
  the server would treat as "no profile". For multibands the fixture now names the responding
  band (`thresholdReview.responding`), so a server rule "the one band that responds at the tone
  is the amount control" would cover most of them without hand entry.
- **Level convention:** ours is peak dBFS; yours sine RMS. For a sine the offset is 3.01 dB and
  the fixture says `level_convention: "peak"`. Convert at the join, never silently.
- **Hold 2.5 s / read 300 ms vs our 1.5 s / 250 ms:** ours doubles the hold when the reading is
  still moving (`holdDoubled`, `stillMoving` recorded) and refuses a position that never settles,
  rather than fixing the hold. Opto units settled within the doubling in every certified case.

## Leads logged, not chased

- Fixture `controls` shorter than the probe's parameter list on 5 of 92 (item 2).
- 1.78 dB appears identically on four Melda candidates across three products — a shared code
  path or fixed step, not four measurements.

## v1.1 (1 Oct, evening): map_fp verified, the exporter, and your section 6 on real data

**map_fp, full 64 hex.** Every record now carries `param_count` and the full `map_fp`. Checked
against reality two ways: 96 of 96 records that have a local EJ Map map reproduce the map's `fp`,
and **91 of 91 records that EchoJay itself has loaded on this Mac carry the fp EchoJay computed**
(its persisted identity→fp index at `~/Library/EchoJay/chain_fp_scan.json`, the source of
EJDialSummary's `fp=`). We are computing your key. `plugin_id` + `version` stays the fallback for
any record from before the field existed.

**Exporter.** One function, `EjmapProfileExport.h`, from a certification record to
`ej_comp_profile/1` (v1.1 fields: 64-hex `map_fp`, `measured.reference_ratio` = the READ-BACK
ratio, `stepped` on stepped amount controls). Every dBFS value is converted from our peak
convention by −3.0103 dB — pinned against the constant and measured from a committed trace
(the probe prints −27.0103 dB RMS beside a −24 dBFS peak hold). A record that cannot fill a field
honestly is refused with the reason; nothing is padded. First real files: after the profile
sweep below (today's 3-level sweeps have no two-quiet-level reference and too few 1 dB crossings
for your 9-point rule, so the exporter refuses all 103 — correctly).

**Your section 6, run on measured curves** (CL 1B, MCompressor, EMO-D5 (s) Comp Thresh; L = −18 dBFS
RMS, g = 2, R = the read-back ratio; GR at L read off the measured curve):

| product | R | your T | position picked (both rules) | eff at it | measured GR at L | readings at that position (peak −24/−12/−6) |
|---|---|---|---|---|---|---|
| Tube-Tech CL 1B | 6.0 | −20.4 | 3 ('0.2') | −22.4 | **1.48 dB** | 0.50 / 1.80 / 3.52 |
| MCompressor (Hard knee) | 1.8 | −22.5 | 5 ('−18.7 dB') | −22.2 | **1.89 dB** | 0.00 / 2.52 / 5.58 |
| EMO-D5 (s) Comp Thresh (Normal knee) | 3.0 | −21.0 | 10 ('−16.0') | −22.4 | **1.95 dB** | 0.01 / 2.59 / 6.44 |

Three things the numbers say:

1. **Your rule lands near the target (1.5–2.0 dB for g = 2), not at g + 1 = 3.** The algebra
   ("eff is where GR *reaches* 1 dB, T is where it *starts*, so matching them over-shoots by 1 dB")
   assumes GR climbs at (1 − 1/R) above the crossing. It does not: CL 1B climbs **0.11 dB per dB**
   over the 4.4 dB above its 1 dB crossing (textbook 0.83); the others likewise. Soft knees, exactly
   the signature in the ratio-free vs R-based disagreement (88 of 117 positions). So the +1
   correction (K = eff − 1/(1 − 1/R)) is smaller than the knee error it would correct, and on
   these three it changes nothing.
2. **The 16-position grid cannot discriminate the two rules**: eff values sit 3–6 dB apart, so the
   corrected K and your T pick the same position every time. The 2 dB-step profile sweep is what
   separates them. Numbers above are interpolated on a 3-level sweep and are the reading, not a
   verdict.
3. **Recommendation, from the data rather than the algebra:** pick the amount position by the
   **measured GR at L on the curve itself** — the profile carries GR at every level for every
   position — not by threshold arithmetic. `eff_threshold_dbfs` stays as the summary number;
   the position choice should read the curve. Your `+knee_db/4` term did not enter here (CL 1B has
   no knee control; MCompressor 'Hard'; EMO-D5 'Normal' prints no dB) — which is itself the finding:
   the knee the curves show is not a control value we can read.

## v1.3 → v1.4 (1 Oct, night): built to v1.4, the contract as of 18:51

`docs/COMP_PROFILE_SPEC_v1_4.md` is what the exporter is built to. Folded in, pinned both ways:

- **`detector_f` required** — a record without the two-tone measurement is not exported.
- **`quality.point_error_db` from the hold-doubled repeat** (2.5 s vs 5 s, `quality.method` says so);
  the identical rerun is dropped. A record without the repeat is not exported.
- **Monotonic self-check, exactly your rule, before export:** within a position 1 < 2 < 3 strictly;
  across positions one direction, nulls skipped, equal neighbours allowed. Pinned: equal
  neighbours pass, a dip fails, an equal 2 dB point fails. The export says the result in
  `quality.monotonic_*` and names violations — you reject, we say first.
- **`notes` lists the guards that passed:** tone_frac (with the count of readings refused as not
  the tone), level dependence, the quiet-reference 6 dB self-check (positions passed / total),
  ascending-only levels per fresh process, the still-moving rule.

Two things you should know from the measurement side:

- The ratio **norm** is not in the profile — only the control name and the read-back value. The
  tone check here takes it from our record's preconditions. If the server is to write the ratio,
  the profile needs the norm (or the display text to write). Suggest `ratio.norm` beside `value`.
- **EMO-D5 is dropped from this Mac's run** until Waves 15 replaces 12: the V12 fingerprint
  (62995254f7dd…) will never match your 15.0.70 map (32b7e1d9a0c3…), and V12 here runs bridged
  where V15 is probably native — a V12 sweep does not rehearse V15. CL 1B goes ahead: our
  `map_fp` ab70ea5337fe… is the fp your session logged on 30 Sep.

## 2 Oct, morning: CL 1B exported to v1.4 — and what the first live profile run taught

`~/Desktop/ej_profiles/ej_comp_profile/Tube-Tech_CL_1B_2.5.62.json` (+ `.tonecheck.json`), also in
`tools/ejmap/profiles-export/`. The numbers:

| field | value |
|---|---|
| map_fp | ab70ea5337fe3b0637ccd409982989fb0815d65a8e0ec9bef2ef9c228dcb5561 |
| reference_ratio | 6.0 (as instantiated) |
| detector_f | 0.40 — sine 2 dB at −10.9, two-tone at −12.1 peak-equivalent; "unknown" by your rule |
| in_at_gr["2"] numeric | 13 of 16 (the three quietest positions sit at or below the sweep's −60 peak floor: null) |
| quality.point_error_db | 0.1 (hold 2.5 s vs 5 s, 36 points, 0 shape disagreements) |
| monotonic self-check | pass, within and across |
| fit.max_error_db | 2.68 (informational) |
| tone check, L = −18 RMS, g = 2 | **1.33 dB at the rule's pick → FAIL** (bar 0.5) |

**Three things from the measurement side, in order of what they cost you:**

1. **Your section 6 clamp and your bracket cannot both hold on this device.** In sine RMS, position
   3's 2 dB point is −13.9 (4.1 dB above L) and position 4's is −23.2; position 4's 1 dB point is
   −29.9, 11.9 dB below L, so the 8 dB clamp removes it and position 3 is picked alone — nothing to
   interpolate toward — and at L it gives 1.33 dB. On a 6:1 soft-knee opto the 1 → 2 dB span is
   7 dB and the next position's 1 dB point is 9 dB lower. Measured beside the rule, not in it: the
   clamp-free interpolation between 3 and 4 (norm 0.2077) gives 1.58 dB, the midpoint (0.2333)
   2.37 dB. Suggest the clamp read the *2 dB* point (the g you are picking for), or widen to the
   position spacing; either is your call, we report the rule's number.
2. **The quiet reference needed a ladder.** CL 1B's threshold reaches −57 dBFS peak; at −48 it is
   already compressing on 10 of 16 positions, so the fixed −54/−48 pair failed its own 6 dB check
   there and the first export refused ("3 points reach 1 dB; needs 9"). The reference now descends
   −54/−48 → −66/−60 → −78/−72 → −90/−84 until a position's pair differs by 6 dB; the record says
   which rung each position used. Two runs 14 minutes apart agree on every rung and every point to
   0.1 dB. Any device whose range goes below −45 dBFS peak will need it.
3. **`notes` now says "reference ladder: N positions referenced below −54/−48"** so you can see it.

Also fixed this morning, both older than the spec: a one-position process (your detector and
tone check) derived nothing, so no live `detector_f` had ever been recorded; and
`plugin.manufacturer` was empty on a record made by discovery. Neither changes a number above.

## 2 Oct, 10:37: CL 1B v1.4 re-exported on a refined grid — PASS, and two things about the clamp

`~/Desktop/ej_profiles_CL1B_v1.4.zip` (profile, tonecheck, README, the Mac runbook). Changes since the
08:35 export, all measured again:

| field | value |
|---|---|
| amount curve | **28 positions**: the 16-point grid plus 12 added wherever adjacent 2 dB points differed by more than 3 dB (two rounds: 11, then 1) |
| in_at_gr["2"] numeric | 25 of 28 |
| ratio | `curve: [{norm 0.5, "6:1", value 6.0, measured_ratio 1.74}]`, `fixed: null`, `knee_db: null` — the norm is what your server writes; measured_ratio is implied from level dependence and says so in notes; no knee was measured, so none is claimed |
| neutral | Gain 0.0 (0.33), Attack 5.0 (0.5), Release 5.0 (0.5), Select Attack Release Man (1.0), Sidechain Int (0.0) — every control except the amount, the ratio and the meter, at the value it was measured at |
| detector_f | 0.43 |
| quality.point_error_db | 0.1 (72 points) |
| tone check, L = −18 RMS, g = 2 | **1.85 dB → PASS**, writing exactly what your server will: the five neutral controls, Ratio 0.5, then Threshold at norm 0.2167 |

**Two things about section 6's clamp, from the measured curve:**

1. **The clamp is checked on the bracketing positions, not on the interpolated pick.** On the 16-point
   grid position 4 was excluded (its 1 dB point 11.9 dB below L), so no interpolation was possible —
   but the point the interpolation would have landed on, between 3 and 4, has its 1 dB point only
   4.5–6.0 dB below L (measured now at norms 0.208 and 0.217: −22.5 and −24.0 RMS), inside the clamp.
   On the refined grid the same thing recurs one step finer: the neighbour at 0.233 has its 1 dB
   point 8.5 dB below L and is dropped by 0.5 dB, so the pick is still a single position. Suggest
   applying the clamp to the interpolated pick (its own 1 dB point, interpolated like its norm)
   rather than to each bracketing position.
2. **On CL 1B the clamp caps the GR the server can ever ask for at about 2.3 dB.** Measured over the
   22 positions with all three points: the 1 → 2 dB spacing is 6.7 dB (6.3–6.8) and 1 → 3 is 10.3 dB
   (9.7–10.6). A position whose 1 dB point is 8 dB below L therefore sits at about 2.3–2.4 dB of GR at
   L; a pick for g = 3 finds nothing inside the clamp on this device. If you want g = 3 reachable on
   soft-knee units, the clamp needs to scale with the measured 1 → g spacing (it is in the profile).

## 2 Oct, afternoon: the server catalogue files every tuner as `no_dial_set` — a fresh Mac finds no tuners

Found by the stranger's-Mac test (an empty ledger, `--scan --categorise`, the census): the categorise
endpoint serves verdicts dated **5 Aug**, and on that date no product carried `category: pitch`.
The 26 Aug re-categorisation on the operator's Mac (local tool `categorise/1`) gave the real-time
tuners `pitch / sweep`, but that file never reached the server. So every tuner is `category null /
no_dial_set` on the server, discovery sees no category, and tuner certification has nothing to do on
any Mac but the operator's. **No server data was changed; this is yours to update.**

What the eight real-time tuners need is `category: pitch` (the client's discovery then puts them on
the tuner worklist; disposition no longer matters — ruled 2 Oct, only `operator_excluded` and the
hang/crash family exclude). Current server verdict → needed:

| product | server now (5 Aug) | kind the server itself wrote | needs |
|---|---|---|---|
| Auto-Tune Pro (Antares) | null / no_dial_set | pitch correction | pitch |
| Auto-Tune Artist (Antares) | null / no_dial_set | pitch correction | pitch |
| Auto-Tune EFX+ (Antares) | null / no_dial_set | pitch correction | pitch |
| Auto-Tune EFX (Antares, 9.0.1) | null / no_dial_set | pitch correction | pitch |
| Auto-Tune Access (Antares) | null / no_dial_set | pitch correction | pitch |
| UAD Antares Auto-Tune Realtime | null / no_dial_set | pitch correction | pitch (hardware-held on a Mac without a UAD-2, but categorised) |
| UAD Auto-Tune Realtime Access / Advanced / X | null / no_dial_set | pitch correction | pitch (same) |
| bx_crispytuner (Plugin Alliance) | null / no_dial_set | pitch correction | pitch — measured 1 Oct: a strength curve 0 → 1.07 |
| MetaTune (Slate Digital) | null / no_dial_set | pitch correction | pitch (PACE; licence-held without the iLok) |
| Waves Tune Real-Time (Waves) | null / no_dial_set | pitch correction | pitch (not installed here; on a Waves Mac it would be the tuner) |
| MAutoPitch (MeldaProduction) | null / no_dial_set | pitch correction | pitch |

Not tuners for this purpose, and correctly not `pitch`: Melodyne, Waves Tune, Waves Tune LT (ARA /
offline — the harness refuses them by name anyway); the pitch *shifters* and harmonisers (Little
AlterBoy, MicroShift, SoundShifter, Torque, UltraPitch ×6, Vocal Bender, Harmony Engine,
MHarmonizerMB, MTransformer, MUnison, Pitchwheel, kHs Pitch Shifter, UAD H910, Fault, Crystallizer);
the instrument tuners (GTR Tuner, MTuner, bx_tuner, UAD bx_tuner — `not_a_processor`, right).

The server's own `kind` field already says "pitch correction" for exactly the right eleven — the
category just never followed. Until it does, **"tuners on a fresh Mac" is BLOCKED on this catalogue
fix** in the pass criteria, and the operator's Mac stays the only place tuner records are made.

## 2 Oct, night: v1.7 built — deep points, the amended pick, the deep tone checks (held until v1.8 lands)

Built on top of `36397676` (Sean's build commit, untouched): `9ed3ba81` deep points, `9d932c7e` the pick,
`03e600eb` the tone checks, `741689b5` the tone-check-only mode (`docs/SEAN_MAC_TONECHECK.md`).
Nothing re-measured: the probe already renders every level to −3.01 RMS at every position and the
straddle reads the whole curve, so 4/5/6 are derivation + export, and the batch's traces (kept in
`cert/raw` with the hold-doubled repeat) let a cert folder be re-derived afterwards.

**CL 1B, re-derived from its committed traces:** deep points on 25 of 28 positions at 4, 5 and 6 dB;
`deep_point_error_db` 0.10 over 75 surviving deep points; nothing nulled by the hold test; monotonic
within and across at every level. At norm 0.2167: 1→6 dB at −24.0 / −17.2 / −13.5 / −11.5 / −9.7 / −7.9
RMS. **The new g = 2 pick** (12 dB clamp on the pick itself): norm 0.2217, interpolated between the
positions whose 2 dB points bracket L (−17.21 / −19.81), its own 1 dB point −24.77 — 6.8 dB below L —
where the old rule picked 0.2167 alone (its neighbour dropped by the 8 dB clamp) and measured 1.85 dB.
**Its tone checks could not run here: the iLok is with Sean**, so the probe showed PACE's window after
2.9 s and was killed — on Sean's Mac the tone-check-only mode runs them from the same traces.

**Measured on four licence-free products here (17 s for all four):** every deep level within 0.05 dB of
its target — bx_opto 4.00 / 5.01 / 6.01, Lindell 7X-500 4.05 / 5.00 / 6.02, elysia mpressor 3.98 / 5.00 /
6.02, Lindell SBC 4.00 / 4.99 and its 6 dB level **nulled**: at L = −18 the only settings reaching 6 dB
have their 1 dB point 14 dB below L, the clamp refuses, the level cannot be confirmed and comes out of
the profile (the profile stands at 1–5) — section 8 as written.

## 3 Oct: the tone check's test level per level — and what the 12 dB clamp does to deep levels

Each level is now tested at its own L (the median of `in_at_gr[g]` over the positions that carry g, then
nearest values, within the sweep's range; first L whose §6.4 pick passes the clamp), recorded per level
in `tone_check.deep_levels[].L_rms_dbfs` with `L_rule`. Result on the four rehearsal units: every level
with a valid L passes within 0.12 dB, including Lindell SBC's 4 and 5 dB that a fixed −18 had nulled.

One thing for v1.8, with numbers: the 12 dB clamp on the pick's own 1 dB point makes a level untestable
at ANY L once the unit's 1→g spacing is ≥ 12 dB at every position. Lindell SBC 1→6: 14.1 dB everywhere.
Tube-Tech CL 1B 1→5: 13.5–14.5; 1→6: 15.3–16.2 (1→4: 11.7–12.6, so some positions are inside). These are
soft-knee / low-ratio units doing what they do; the profile now nulls such a level with the reason
`no_valid_L_clamp_geometry` and the spacing, rather than silently. If you would like those levels
tested, the clamp needs to grow with g (e.g. 12 + (g − 1) dB, or read the pick's own (g − 1) dB point
instead of its 1 dB point); we have not changed §6.4 on our side.

## 3 Oct: v1.8 received — the reverse read is built

§6.4 step 2's reverse read (GR at a level across all six points, past the deepest flagged extrapolated) is in the pick
replica; a stepped pick now expects what its detent gives at L (step 6). Pinned, with the 1–3-only read as a red mutant.
No profile we have exported is stepped, so no existing tone-check verdict changes.

## 3 Oct: the clamp for deep asks (v1.9, your ruling of 3 Oct) — built, SBC's 6 dB now tested

12 dB up to g = 3, then 12 + 2 × (g − 3) (14/16/18 at 4/5/6, linear between), the same comparison, in both the pick
replica and the tone-check L rule; pinned at 2/3/3.5/4/5/6 with a mutant proving nothing widens below 3. Lindell SBC's
6 dB level: GR 5.98 at L −13.16 (its 1 dB point 14.1 below, inside 18) — tested, not nulled. CL 1B's rule L at 4/5/6:
−31.61 / −29.81 / −28.01 (12.0 / 13.8 / 15.6 below its 1 dB point); the checks run when the iLok is with the Mac. We will
check this wording against v1.9 when it arrives.

## 3 Oct: notes — every deep null accounted for; the shape question

Every deep null in an export is now on exactly one `notes` line, `deep null <g> dB - <reason>: positions <norms>`, with
the reason from the record (not reached by −3.01; past at the quietest level; no rising straddle; hold test failed, both
values; the all-null position; the tone check's own two). One question: §3 says `notes` is a list of plain strings, your
example still shows `""`, and our exporter writes one `"; "`-joined string. Which does your validator accept? Ours flips
with one constant either way.
