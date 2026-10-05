# Overnight 4/5 Oct 2026 — morning report

**The follow-up build for Sean is commit `17ebf114`** (branch `feat/ejmap-cert`; named in `PACKAGING_EJMAP_APP.md` and
`SEAN_MAC_TONECHECK.md` by 3dd4b2c8). 36397676 untouched. Everything after 17ebf114 is Phase B: new modes only, nothing in the
certification path, nothing exported, nothing published, nothing sent. Suite 3443, every commit gated.

## Phase A — compressors and tuners (done)

| item | what | commit | pins / mutants | live |
|---|---|---|---|---|
| A1 sidechain | the probe leaves every input element past the main one **unconnected** after `prepareToPlay` (Logic's state); policy and record say `unconnected` | db83b0fb | S1–S14 / M9–M13 | C1 comp 0 → 10.46 dB GR, RComp 0 → 54 dB; SBC, mpressor, EMO-D5, bx_opto, 7X-500 unchanged to 0.0001 dB |
| A2 evidence-based re-sweep | for every record swept under `enabled_silent` with a second input bus, ONE reading under the new policy against its own trace; ≤ 0.1 dB keep, else re-sweep; crash/window on the row | db83b0fb | same | C1 comp (s) swept under the OLD app → follow-up read −9.01 → −9.54 → re-swept → certified → exported with its tone check |
| A3 inert | Power/bypass and gain-type controls written once against a control reading; none moving the output → `sweep result inert: processing never runs … including Power` | db83b0fb | I1–I9 / M14–M16 | V76U73 inert in 3 s |
| A4 tuner plan v2 | detents from the READ-BACK values (not the "landed" flag: Access's Key gives 12 keys from 33 writes of which 4 "landed"); speed half period 1 → 2 → 4 s per unsettled position; `planDiffers` re-measures v1 tuners | 17ebf114 | T1–T10 / M17–M20 | Access 245 / 117 ms at Slow / Medium (4 s), Fast a bound; Pro 400 → 1099 ms; Artist 400..69 → 1339..256 ms at 4 s |
| A5 v0.1 measurements | flex ladder 5/10/20/30/40/45 (never 50); the **window** statistic; Humanize held vs short notes; key/scale read-backs; `--tuner-profile-draft` → `ej_tuner_profile/1` "PROPOSAL v0.1 - not for publication" | 17ebf114 | V1–V12 / M21–M24 | Pro Flex-Tune 0 → everything corrected, 29 → 40 c, 57 → 20 c, 86 → 10 c, 100 → nothing; Humanize moves steady strength 3 % only; Key 12, Scale 29 |
| A6b Sean's stepped rule | detent norms as read back, only detents; every detent hold-tested; `at_control_limit` past the ends; ≥ 3 detents reaching 1 dB; the plan sweeps a control with landing evidence at its detents; the follow-up runs the landing read FIRST | 17ebf114 | Z0–Z12 / M25–M29 | pinned on Lindell 254E's own 16-detent record |
| A6 projection | over Sean's zip: **20 re-sweeps** (14 compressors + 6 tuners), 48 sidechain readings, 8 inert checks; 45 export now, 13 pending detector; ≈ 1 h 30 – 1 h 45 on Sean's Mac | 3dd4b2c8 | — | packaged app's projection identical to the build tree |
| A7 build | re-cut on 17ebf114; pre-flight; four rehearsal units 40 / 40 with no `--probe` (landing reads and sidechain readings live inside) | 3dd4b2c8 | — | yes |

The `inert` label now also names a second cause: **every inert unit so far is a Plugin Alliance product** (V76U73, U2A, U17, HG-2,
bx_saturator V2) on a Mac where other PA products process — a per-product licence state, silent, no window.

## Phase B — prototypes and proposals (new modes only; nothing goes to Sean from these)

| item | mode | commit | measured here | the catch |
|---|---|---|---|---|
| B1 gain / output | `--cert-gain-cal`, docs/GAIN_CALIBRATION_PROPOSAL.md | 4f50813f | 14 compressors | Solid Bus Comp Makeup 1.00 dB off, bx_townhouse MakeUp 6.80 dB off; the rest honest; labels judged at their own resolution; input gains inside the compression path are level-dependent |
| B2 timing | `--burst` in the probe, `--cert-timing`, docs/COMPRESSOR_TIMING_PROPOSAL.md | 050b1a89 | 7 units | attack/release track labels, each unit by its own definition (mpressor's release label IS the 63 % time; 254E's ~2×; RComp's ~0.5×); SBC program-dependent; latency alignment was the key fix |
| B3 limiters | hold line gains sample + true peak; `--cert-limiter`, docs/LIMITER_PROPOSAL.md | 2b7c998a | 4 limiters | **MLimiterX overshoots true peak by +0.58 dB at every ceiling** (sample peak exact); L2, bx TP, Elevate hold; Elevate's True Peak switch = 0.14 dB headroom |
| B4 EQ | `--response` 121-tone multitone, `--cert-eq`, docs/EQ_PROPOSAL.md | 312894a9 | 5 EQs | bx_digital labels within 0.5 %, proportional Q; AMEK EQ 200 centres 4–10 % below label; PEX-500 "Low boost 0–10" is not dB (10 → 15 dB) and 30/60/100 Hz are turnovers (corners 211/309/663 Hz); BAX labels 1.2×; second-channel bands flat (engage search needed) |
| B5 saturation | `--cert-saturation`, docs/SATURATION_PROPOSAL.md — **partial** | 7930a4f0 | 2 of 4 | the sweep's `tone_frac` floors the THD proxy at −40 dB; harmonic Goertzel bins proposed, not built; HG-2 and bx_saturator inert |

## Waiting on Kathy

- `review_picks.json`: your initials, and Shadow Hills Discrete or Optical. Untouched tonight.
- The MaxxVolume re-sweep stays literal (your ruling): 2 × ~4 min for a record that ends in review either way.
- Whether the PA licence shape (`inert`) should become its own state or stay a `needs_review` reason.
- B1–B5 are proposals: which, if any, go to Sean, and in what order.

## Waiting on Sean

- Logic checks: MDynamics, Low Control, Pro-C 3, C1 comp (now explained: it compresses once the sidechain is unconnected — the
  Logic test in COMP_PROFILE_REPLY.md is no longer needed for the four Waves units), **V76U73 and the other PA units**: open one
  in Logic on each Mac and see whether it processes (licence state).
- UnFairchild's stepped minimum: built per his four conditions (A6b), noted in COMP_PROFILE_REPLY.md as "agreed by Sean 4 Oct".
- The multiband proposal (docs/MULTIBAND_PROFILE_PROPOSAL.md) — unchanged.
- New: the tuner v0.1 questions (flex **window**, Humanize needs a timing measure, 50 cents is never a rung); the stepped-rule
  spec text (v2.2); where gain calibration lives; the limiter's true-peak question; the EQ shape/field question.
- CL 1B's Gain (0.33 must read 0.0): needs the iLok — `--cert-gain-cal "Tube-Tech CL 1B"` on his Mac.

## Blocked / not done

- Auto-Tune EFX 9.0.1 here refuses `--list-params` (exit 3); Sean's 9.5.0 measured. Not chased.
- Auto-Tune Artist's Retune Speed 36 / 17 / 6 never settle even at a 4 s half period: recorded as refused with the reason; a lead
  (a drift or oscillation, not a time constant).
- SSL X-Limit, kHs units: PACE-bound here (a window; the watch killed the load). Skipped.
- B5 saturation: proxy only; the harmonic measurement is designed, not built.
- Phase B modes measured 2 of the roadmap's "every exported compressor" for gain calibration (14 here); the rest are Sean's Mac.

## What I'd do next

1. Sean runs the follow-up from 17ebf114 (SEAN_MAC_TONECHECK.md); zip back; project.
2. Fold the proposals' catches back into certification only after rulings: the EQ band engage search, the timing hold scaled to
   the attack label, the gain-cal third level for input gains, the limiter drive level.
3. Build the harmonic bins for saturation (small, in `--response`).
4. Decide the PA-licence shape once Sean has opened V76U73 in Logic.

Traces: `tools/ejmap/cert-traces/2026-10-04-sidechain/`, `2026-10-04-stepped/`, `2026-10-05-timing/`; the Phase B JSONs are in
scratch (`phaseb/*`), not committed (prototypes). Memory notes updated per phase.
