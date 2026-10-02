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
