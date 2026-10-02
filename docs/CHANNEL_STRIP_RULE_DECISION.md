# The channel-strip rule — laid out for a ruling (2 Oct 2026, evening; NOT built)

Today a record with several threshold candidates ends `needs_review`. Every candidate is swept with the
others at their instantiate values (that is how the table below exists), and nobody picks. On EMO-D5 (s)
that costs 43 minutes to say needs_review. Three rules, exact wording, then every product each would
decide, then the risks.

## The rules, exactly

**Rule 1 — the compressor stage word.** *Among a product's threshold candidates, exactly one has a name
token that begins "comp" (Comp, Compressor, Compression; a token is a word split on space, dash, slash,
arrow, bracket), and that candidate's own sweep certifies with every other candidate left at its
instantiate value. That candidate is the amount control; its engage write, if its sweep needed one,
rides with it; every other candidate (and every other control) is a neutral entry at its instantiate
value. Otherwise: needs_review.* — Applied at PLAN time: only the comp-worded candidate is swept.

**Rule 2 — the single certifier.** *No candidate carries the word, exactly one candidate certifies, and
no candidate name is band-shaped (Band N, Low / Mid / High, L / M / H, LF / MF / HF / LMF / HMF) or
stage-numbered (Processor N). That one is the amount control.* — Needs every candidate swept first
(today's cost), because "exactly one certifies" is only known afterwards.

**Rule 3 — channel twins.** *Exactly two candidates whose names differ only by a channel suffix (L / R,
1 / 2, A / B, L/M / R/S, Left / Right, "" / " R"), and BOTH certify. The first is the amount control and
the second is recorded as its twin, so the server writes both.* — Also needs both swept.

## What each rule decides, product by product (from the store today)

| product | rule | pick (its sweep; its engage write) | the other stages, left at their instantiate value | why the pick is the compressor |
|---|---|---|---|---|
| EMO-D5 (s) 12.0.0 | R1 | **Comp Thresh** (certified; engage `Comp On → 1.0`) | Gate Thresh = certified, left at −Inf with `Gate On` Off; Leveller Thresh = certified (run 2) / unreadable (store), left at 0.0 with `Leveller On` Off; DeEsser Thresh = flat, left at 0.0; Limiter Thresh = certified, left at 0.0 with `Limiter On` Off | the one stage named Comp; the other three "…On" switches stay Off, so their stages are out of the path. **This is exactly the hand run's choice on 1 Oct** (`--candidate "Comp Thresh"`, engage Comp On) |
| EMO-D5 (m) 12.0.0 | R1 | Comp Thresh (certified; Comp On → 1.0) | as above | as above |
| Solid Dynamics 1.4.5 | R1 | **Threshold Comp** (certified) | Threshold G/E = flat, left at 0.0 dB | Comp vs G/E (gate / expander): the word, and the G/E stage does nothing to a 997 Hz tone at 0 dB |
| MDynamics 14.16.0 (PACE) | R1 | **Compressor → Threshold** (certified) | Gate – Threshold = flat, left at −28.0 dB; Processor 1 – Threshold = **certified**, left at −20.0 dB; Processor 2 – Threshold = flat, left at −24.1 dB | the stage Melda names Compressor. Note: Processor 1 ALSO certifies at its default, so the exported curve is the product's GR with Processor 1 active at −20 dB — consistent with what the server writes (neutral keeps it there), but it is two stages' GR, not one |
| Unfiltered Audio Zip 1.4.2 | R2 | **Threshold** (certified) | Auto Threshold = flat, left at 'Disabled' | the only certifier; "Auto Threshold" is a mode switch |
| PuigChild 670 (s) 12.0.0 | R2 | **Left Threshold** (certified) | Right Threshold = flat, left at 2.2 | the only certifier — **but see the risks: Right is flat because the probe's tone is on the left** |
| Auto-Tune Vocal Compressor 1.5.0 (PACE) | R2 | **Mod Comp 1 Thresh dB** (certified) | Mod Comp 2 Thresh dB = flat; Opt B Comp 1 / 2 Threshold dB = flat; all left at 0.0 | Rule 1 refuses it (four comp-worded names); the only certifier is the engaged circuit's first stage |
| elysia alpha master 1.17.1 | R3 | **Threshold 1** (certified), twin Threshold 2 | Threshold 2 = certified, left at +20.0 dB | a 1 / 2 pair, both certify: two channels of one compressor |
| DPR-402 (s) 12.0.0 | R3 | **Threshold L/M**, twin Threshold R/S | Threshold R/S = certified, left at 'Out' | L/M–R/S pair |
| Abbey Road RS124 (s) 12.1.0 | R3 | **Input Control**, twin Input Control R | Input Control R = certified, left at 5.0 | the "" / " R" pair (input-as-threshold, both channels) |
| SPL IRON 1.6.1 | R3 | **Threshold L**, twin Threshold R | Threshold R = certified, left at 0.0 | L / R pair |
| AMEK Mastering Compressor 1.1.1 | R3 | **Threshold 1**, twin Threshold 2 | Threshold 2 = certified, left at 0.0 dB | 1 / 2 pair |
| Millennia TCL-2 1.11.1 | R3 | **Threshold 1**, twin Threshold 2 | Threshold 2 = certified, left at 0.00 | 1 / 2 pair |
| Vertigo VSC-2 1.15.1 | R3 | **Threshold A**, twin Threshold B | Threshold B = certified, left at +11.8 dB | A / B pair |

Rule 1 decides 4; Rule 2 adds 3; Rule 3 adds 7 (fourteen in all; my earlier count said 4 + 2 + 9 — the
mechanical application above is the one to trust, and it moves Auto-Tune Vocal Compressor into R2 and
leaves Maag MAGNUM-K out of R3 because it has six candidates, not two).

**Stays needs_review under all three (28):** every multiband (LinMB ×2, C4 ×2, C6 ×4, MDynamicsMB ×2,
MTurboCompMB, OTT, DynOne3, Lindell 354E / MBC, SSL G3, Drawmer 1973), dbx-160 (s) (nothing certifies),
MDrumLeveler, Pro Audio DSP DSM V3 (three certify), Kiive XTComp (three input controls certify), Maag
MAGNUM-K, MSpectralDynamics ×2 (Processor 1 of 2), MaxxVolume ×2 (Low Level / High Level: two stages, both
certify — a human knows High Level Thresh is the compressor; no rule above does), Shadow Hills ×2
(Optical / Discrete: two circuits in series, both certify).

## EMO-D5 specifically

Rule 1 picks **Comp Thresh with `Comp On → 1.0` as the engage write — the same choice the hand run made
on 1 Oct** (the Desktop export for Sean was made with `--candidate "Comp Thresh"` and that engage).
Cost, from run 2's process timings: the Comp Thresh path alone is 84 s (first pass, pass-through) +
90 s (its hold-doubled repeat) + 29 s (the engage probe and its repeat) + 154 s (the engaged sweep with
grid refinement) + 171 s (its repeat) = **~530 s ≈ 9 minutes**, against 2589 s today. Skipping the
hold-doubled repeat of a first pass that read pass-through (a flat sweep repeated proves nothing) takes
it to ~7.5 minutes. The DeEsser candidate alone cost ~400 s today trying fourteen engage switches.

## Where a pick looks plausible and could be wrong

1. **A leveller or limiter stage mistaken for the compressor.** Rule 1 cannot make this mistake — it
   keys on the word "comp", so EMO-D5's Leveller Thresh and Limiter Thresh (both certify with a clean
   curve) are never picked. The converse is the gap: a product whose compressor is NAMED Leveler / Level
   / Dynamics (MaxxVolume's High Level Thresh) ends needs_review, which is right.
2. **A stereo twin where the right side reads flat.** PuigChild 670 (s): Rule 2 picks Left Threshold
   because Right Threshold is flat — flat because the probe's tone is on the left channel, not because
   the right stage is off. The server would then write only the left threshold and the right channel
   would not compress. Rule 3 cannot see it (the twin did not certify). Proposal: an L / R (or Left /
   Right) pair whose right side is flat is a TWIN, written together — or it stays needs_review. Not
   Rule 2 as worded.
3. **Two stages both active at their defaults.** MDynamics: Processor 1 certifies at its default
   (−20 dB), so the Compressor curve includes it. Consistent for the server (neutral holds Processor 1
   at −20 dB), but the profile is the product's GR, not the compressor stage's. Worth a `notes` line.
4. **Two circuits, both certifying.** Shadow Hills Optical / Discrete and Auto-Tune Vocal Compressor's
   Mod / Opt B: Rule 2 picks the engaged one (the other is flat at defaults), Rule 3 would wrongly call
   Optical / Discrete twins if both certified — they are not channels. Rule 3's suffix list must stay
   literal (L/R, 1/2, A/B, L/M–R/S, Left/Right, ""/" R"); "1" vs "2" on Shadow Hills are stage numbers
   of DIFFERENT circuits and happen not to pair because the names differ in more than the suffix.
5. **"Comp" inside another word.** The token rule ("begins comp") would match "Compare" or "Component"
   if a plugin named a control so; none in the store does. The rule should be a whole-token match on
   the three words, not a prefix.

## What I would need from the ruling

Which of R1 / R2 / R3 to build; whether R1 runs at plan time (only the comp-worded candidate swept —
the 9-minute EMO-D5) or after a full sweep (today's cost, but the full review table survives); the
PuigChild twin question in risk 2; and whether a product decided by a rule exports with a `notes` line
naming the rule and the stages left at default (I would).

## The ruling (2 Oct, evening) and what was done

- **Rule 1 — BUILT, at plan time** (`130cbf9d`): whole-token match on exactly comp / compressor /
  compression (a prefix match makes a "Compare" control go red: pin K2/K5, mutant R1-M1); the pick is
  swept first, alone; a miss falls back to the full table; rule-decided exports carry a `notes` line
  naming the rule, the pick, its engage write, every other candidate and control at its instantiate
  value, and any stage active at the defaults (the defaults reference's GR, with the candidates left at
  a level named — which of them is not measured individually under the plan-time rule). The
  hold-doubled repeat is skipped only for a pass-through first pass (K7, mutant R1-M3). EMO-D5 pinned
  (K3) and run live below.
- **Rule 2 — REJECTED.** Zip was a roles bug: "Auto Threshold" (Disabled / Enabled) is a word-valued
  mode switch; a threshold-named control whose sampled values are words is never a candidate (Z1–Z2,
  two mutants), so Zip is single-threshold. Auto-Tune Vocal Compressor stays needs_review.
- **Rule 3 — accepted in principle, NOT built**: Sean's v1.4 has one amount control; a twin exported
  today would make the server write the left threshold only. Needs a spec field (Kathy is asking Sean);
  then two guards — the twins' in_at_gr curves agree within the 0.5 dB point_error gate or
  needs_review, and the tone check writes BOTH and measures GR on BOTH channels. The seven R3 products
  stay needs_review.

## PuigChild 670 (s): why Right Threshold reads flat — reported, nothing changed

From the traces (`cert-traces/2026-09-30-batch5b-candidates/raw/AudioUnit_4c45797d_12.0.0.sweep.*`)
and the defaults sample:

- **The probe drives both channels.** Every hold logs per-channel output (`ch[L,R]`) and the two are
  identical in every process of both candidates (e.g. −25.0950 / −25.0950 at −24 dBFS). With **Left
  Threshold at its maximum, BOTH channels compress identically** (−13.22 dB out−in at −6 dBFS on L and
  on R). So the right channel receives the tone and is processed — there is no routing bug, and SPL
  IRON / Vertigo / DPR-402 certified their right sides for the same reason.
- **Right Threshold's writes land and change nothing on either channel.** At norm 0.0 and at 1.0 the
  outputs are the same to 0.01 dB (1.91 / 1.05 / −1.89 dB, the defaults' own level dependence with Left
  Threshold at 2.2) and `getValue` reads back 1.000000.
- **The cause is on the record: `Link` instantiates at 'Linked'** (0.5; the other positions are
  'Left/Right' and 'Lat/Ver'). On a 670 in Linked mode the left-side controls drive both channels and
  the right-side controls are followers — the plugin models exactly that. "Flat" here means "this
  control is a follower in the instantiate mode", not "the signal did not reach it".

So PuigChild 670 (s) is a single-amount-control product in its instantiate mode: Left Threshold drives
both channels, and an export with Left Threshold alone would be right for the server AS LONG AS Link
stays 'Linked' (neutral holds it there). It is not a Rule 2 case and not a twin. What it is, is a
third shape — a link mode that makes one control the whole unit — and a rule for it would read the
link control's instantiate value, not the right side's flatness. Not built; your call.

## EMO-D5 (s) under Rule 1, live (2 Oct 16:59–17:08; traces `cert-traces/2026-10-02-rehearsal/rule1-emo-d5/`)

`RULE 1: 'Comp Thresh' carries the compressor stage word alone - swept first`; first pass pass-through,
its hold-doubled repeat skipped; engage search: `Comp On -> 1` verified; the engaged sweep with grid
refinement (14 positions added) and its repeat; `'Comp Thresh' certifies - the amount control`.
**Sweep 491 s, whole product 521 s (8.7 min) against 2589 s this afternoon.** Only Comp Thresh was
swept (`thresholdCandidates` holds one). Export: `engage [Comp On -> On]`, 40 neutral entries (every
other control at its instantiate value — Gate On Off, Gate Thresh −Inf, Leveller On Off, Leveller Thresh
0.0, DeEsser On Off, Limiter On Off, Limiter Thresh 0.0 …), detector_f 0.70, tone check **1.97 dB
PASS** writing all 42 controls, and the notes line: *amount control decided by Rule 1 (the compressor
stage word): pick Comp Thresh with engage Comp On -> On; other threshold candidates and every other
control at their instantiate values: Gate Thresh='-Inf', Leveller Thresh='0.0', DeEsser Thresh='0.0',
Limiter Thresh='0.0'; no other stage active at the defaults (defaults reference flat)*.

## Link controls on the twin products (read-only, 2 Oct evening; for the list AFTER Sean's Mac)

Whole-token "Link" control and its instantiate value, from the store's defaults samples:

- elysia alpha master 1.17.1: `Link = 'On'` (Off / On)
- DPR-402 (s) 12.0.0: no whole-token Link control
- Abbey Road RS124 (s) 12.1.0: `Link = 'On'` (Off / On)
- SPL IRON 1.6.1: `Link = 'On'`; also `SC Link = 'Off'`
- AMEK Mastering Compressor 1.1.1: `Param Link = 'On'`; also `Sidechain Link Mode = 'Max'`, `Sidechain Link Amount = '100 %'`
- Millennia TCL-2 1.11.1: `Stereo Link = 'On'`
- Vertigo VSC-2 1.15.1: `Link = 'Stereo'` (Mono / Stereo)
- PuigChild 670 (s) 12.0.0: `Link = 'Linked'` (Left/Right / Linked / Lat/Ver) — the follower case above

Six of eight start linked, so a **linked-stereo rule** (Link held at its instantiate value; the left
control drives; GR measured on BOTH channels) goes on the list for after Sean's Mac. One thing the
rule must measure rather than assume: on the seven R3 products BOTH thresholds certified with Link
On, so on those "Link" links the detectors (or, AMEK's `Param Link`, may mirror a write to one
threshold onto the other — which would make each side certify because writing either moves both),
whereas on PuigChild it makes the right side a follower. Same word, three behaviours; the traces of a
right-side sweep with the left held, and vice versa, tell them apart. Not built.
