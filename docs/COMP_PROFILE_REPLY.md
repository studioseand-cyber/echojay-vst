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
