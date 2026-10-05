# Overnight run 2 (5 Oct 2026) — morning report

**17ebf114 untouched; nothing sent.** 36397676 untouched; `PACKAGING_EJMAP_APP.md` and `SEAN_MAC_TONECHECK.md` not edited
(both still name 17ebf114). Eight commits on `feat/ejmap-cert`, b29b7833 → abe5a9d3, every one through the gate (no
`--no-verify`), pushed. Suite 3443 → 3555 checks. Every live run in scratch folders; `~/Library/ejmap/cert` does not exist
on this Mac; the review's unzip scratch is deleted after each run (47 GB free). No PACE product was loaded on purpose
(MO-TT showed its activation window at `--list-params` and the watch killed it; nothing else asked).

| item | commit | what | the one finding |
|---|---|---|---|
| R1 zip review | 26a56061 | `"$BIN" --cert-review-zip <zip> [--against <zip>] [--keep]` — nine sections: hygiene, outcome counts and every state change by product, the projected re-sweeps RAN / NOT RUN and how each ended, sidechain readings, review picks, tone checks per level, deep points, inert / licence, crashes | the projection is the follow-up's own decision pass run derive-only over the baseline copy (captured, not re-implemented): Sean's zip against itself gives 20 re-sweeps / 48 readings, as last night; the cert_tc35 → cert_sc rehearsal pair reads correctly. **Default baseline = `~/Downloads/ejmap-cert-MacBook-Pro-4-20261004.zip`** |
| R2 inert = licence | 6b437c4e | an inert sweep (single or every candidate) files `needs_licence` with the remedy in the reason; `--retry-licence` re-sweeps it | projection over Sean's zip: needs_licence 10 → 10 — nothing in that zip is inert before the follow-up's own inert check runs; eight flat records are projected to get it, so his count can move by up to 8 |
| R3 saturation | 6164bb34 | probe `--response tones=1 harmonics=5` (exact bins); THD, even/odd character, 1 % / 0.1 % onsets, gain and THD spans at −20 / −12 / −6; shapes inert / **silent** / **no effect** | **every onset is level-dependent** (J37's 1 % point moves from '18.7' at −20 to '4.0' at −6); gain law and THD law are separate things; bx_yellowdrive and freqtube are **silent** — the third Plugin Alliance licence shape (inert passes untouched, silent passes nothing) |
| R4 reverb / delay | 6acd684b | probe `--tail` (+ a host playhead at a tempo); mix law, RT60 (T20 / T30), pre-delay and delay time vs labels, repeats and feedback, tempo sync at 90 / 120 / 140 | **the mix law differs by product**: H-Delay and ValhallaVintageVerb equal power to 0.00 dB, MannyM Delay linear to 0.04, H-Reverb a send law; H-Delay syncs to the probe's playhead exactly; bx_delay2500's feedback % is not amplitude % (−1.5 dB per repeat less fall than 20 log10 at every setting); Abbey Road Plates' pre-delay exact to 0.5 ms; bx_rooMS crashed the probe in `--text-at` |
| R5 transients / gates | e1862a04 | probe `--hits` (drum-like burst) and `--ramp`; shaper transient / sustain vs neutral and vs dB labels; gate open / close / hysteresis / range from the ramp, attack / hold / release from the burst | Smack Attack's Attack is exactly linear ±24 dB; **G8's threshold and range labels are exact to 0.0 dB** and its hold within 4 %; C1 gate opens 0.9 dB above its label with 3 dB hysteresis; the 150 ms hit is too short to read sustain labels (MTransient ±24 reads ±2.9) |
| R6 de-essers | 97e30d0d | the ladder at 6.5 kHz with 997 Hz as the control; the reduction's shape (notch centre or shelf corner) vs the Freq label; split vs wideband | band selectivity is clean on every working de-esser (5–19 dB at 6.5 kHz vs 0.0 at 997); **the Waves "Freq" labels are corners, not centres** (the cut holds to 20 kHz); Lindell 902's HF Only switch is literal |
| R7 multiband | b28355c7 | `--cert-multiband`: per-band ladders at the band centres from the default crossovers, the whole unit on a vocal-shaped multitone (`shape=vocal`), the amount as the global control or a common dB offset | **the offset family works on C4 / LinMB / 354E** (C4 at −24: 5.4 dB on the vocal signal, 5–8 dB per band on its own tone; 354E clamps at its −20 dB end); OTT and Melda need the global control's zero as the open reference and a signed GR (both make GAIN at quiet levels); C6's floating bands and Melda's 22 "thresholds" break the order-pairing — two rule cases to add |
| R8 loose ends | abe5a9d3 | (a) EQ band engage search; (b) timing segments scaled to the label; (c) input gains at −60 too, judged there; (d) Artist 36 / 17 / 6 read from the traces | (d) **neither drift nor oscillation**: the output sits at −0.02 ± 0.00 cents for 3.8 s; the pitch tracker's last window straddles the next flip (+9 cents) and failed the stay check — fixed, pinned, all eight Artist speeds now measure (36 / 17 / 6 → 144 / 85 / 43 ms). (a) bx_digital's "2" bands stay flat with their Active switches on: they are the Side channel, and an identical stereo input has no Side — a signal change, not a switch |

Docs: `docs/EJMAP_CERT_DRIVER.md` §51–53, `SATURATION_PROPOSAL.md`, `REVERB_DELAY_PROPOSAL.md`, `DYNAMICS_TRANSIENT_GATE_PROPOSAL.md`,
`DEESSER_PROPOSAL.md`, `MULTIBAND_PROFILE_PROPOSAL.md` (numbers appended). Traces: `tools/ejmap/cert-traces/2026-10-05-*`.

## Waiting on Kathy

- Run the review on Sean's zip in the morning: `"$BIN" --cert-review-zip ~/Desktop/<his zip>` (his 4 Oct zip must be in
  `~/Downloads` for the default baseline, else `--against <path>`).
- The name rules: every prototype picks its controls by name and every one of R3–R7 found a case the names get wrong
  (Saphira's band gains as drives, bx_delay2500's "Modulation Mix", TransX's "Range", Melda's 22 thresholds, C6's floating
  bands). The honest alternative is to let the measurement decide (a control is a drive when its THD span crosses a bar, a
  time control when the onset moves, …) — a ruling on whether to build that, or keep names plus a per-product override.
- Sean's Auto-Tune Artist row: the settle fix does not re-sweep a record already at `pitchPlan.version 2`; his Artist needs
  `--retry-refused` or a plan-version bump (which would re-sweep every tuner) — your call.
- `review_picks.json` untouched.

## Waiting on Sean

- The five proposals' questions (each doc ends with them): saturation's drive rule and level; the reverb mix as a percentage
  or a wet level; the de-esser frequency figure (corner / centre / label); the gate timing definition; the multiband schema
  (unchanged) plus a signed GR and the global control's zero as the reference.
- The three Plugin Alliance licence shapes on this Mac (inert, silent, and the V76U73 pass-through): one of each in Logic.
- bx_rooMS crashes the probe in `--text-at all` (signal 11): a hand check in Logic, or it stays out of the delay / reverb pass.

## Blocked / not done

- Saturn 2: `--text-at all` timed out at 120 s (FabFilter's slow text reads); not measured.
- Band-limited noise for the de-esser pass: not built — the 121-tone multitone stood in, and it drives two units too softly
  (Sibilance, TB_Sibalance read on the ladder but under 3 dB on the multitone).
- The reverb window: 6 s reads RT60 up to ~8 s; ValhallaVintageVerb at Decay 0.75+ needs a longer `tail_s`.
- Sustain labels on transient shapers need a longer hit (500 ms decay) — the 150 ms hit's body is its own decay.
- AVOX SYBIL refuses `--list-params` (exit 3); smartDeess gives no readings without its learn state; MO-TT is PACE-bound here.
- Two runs lost their JSON to my own harness (Saphira, NLS Buss: `| head` closed the pipe before the write) — the numbers are
  in the run log; the lesson is in memory (never cap a live run's stdout).

## What I'd do next

1. The measurement-decides rule for control roles, once ruled: one pass over every numeric control per category, the name
   rule kept as the first guess.
2. A stereo-decorrelated response run for M/S EQ channels, a longer tail for long reverbs, a longer hit for sustain.
3. The multiband pairing by band word / number (C6, Melda) and the global family's zero reference and signed GR.
4. Fold the R8 catches into the certification path after rulings (none of R3–R8 touches the follow-up's decisions; the
   projection pointer in `runToneCheckAll` is null in every batch).
