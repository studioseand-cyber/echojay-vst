# Run 3 (5 Oct 2026, evening) — report

**17ebf114 untouched; nothing sent.** 36397676 untouched; `PACKAGING_EJMAP_APP.md` and `SEAN_MAC_TONECHECK.md` not edited
(both still name 17ebf114). Four commits on `feat/ejmap-cert`, a57fb83c → this one, every one through the gate, pushed.
Suite 3555 → 3598 checks. Live runs in scratch only; `~/Library/ejmap/cert` does not exist here; no licence-bound product
was loaded (MO-TT's activation window at `--list-params` was the only one, killed by the watch). 47 GB free.

| item | commit | what | the one finding |
|---|---|---|---|
| 1 role by measurement | 50d4f5d8 | one header (`EjmapRoleEvidence.h`) holds every role's signature; every Phase B mode nominates by name, confirms or drops the nominee by its own two ends with the reason on the record, and probes the unnamed numeric controls (at most 40, meters out) with the mode's cheapest two-position process — `measured role, unnamed` when they show it; the multiband pairs each threshold with the band it measurably cuts | **the live runs taught six guards the ruling implies**: a silent end reads as "THD" (J37's Output Level), an input / output gain reads as an "EQ band" (every tone moves alike), as a "ceiling" (it passes a 6 dB drive change a ceiling holds), as a "shaper" (transient and sustain move alike), as a "range" (the open level moves too) and as "feedback" (every repeat lifted over the floor) — each is a pin now. Every run-2 name-wrong case is a pin, five mutants red |
| 2 gaps from run 2 | 26f8cf86 | de-esser noise ladder (4–10 kHz random-phase multitone); the reverb tail 1.5 × the decay label (6..30 s, pinned); the sustain control read on a long hit as well; Saturn 2 read-only | the noise reads the tone's GR within 0.7 dB (10.98 vs 11.72); **the long hit reads sustain SMALLER** (Smack Attack ±7 → ±2.4, MTransient ±2.9 → ±0.9 for ±24 dB) — the sustain window against the unit's own envelope is a question, not a window length; **Saturn 2 is not slow: it has 951 parameters** × 3 samples × 78 ms = 221 s against a 120 s timeout |
| 3 digest | 50258553 | `docs/PHASE_B_DIGEST.md`, nine categories shortest first, two-line method, three findings, questions; the role rule and the licence shapes across all nine | not sent; Kathy decides |

Live evidence: `tools/ejmap/cert-traces/2026-10-05-roles/` (roles1–4.txt over fourteen units: SBC, J37, bx_limiter, bx_digital,
H-Delay, bx_delay2500, H-Reverb, Smack Attack, TransX Wide, MTransient, G8, DeEsser, C6, MDynamicsMB; `saturn2/`).

## What the role step says on the fourteen (the lines that matter)

- **Confirmed where the name was right**: SBC's Input / Output Gain (the same at two levels), J37's Saturation, bx_limiter's
  Ceiling (holds against a 6 dB drive change), bx_digital's band gains / frequencies / LMF Q, H-Delay's Mix / Delay Sec /
  Feedback, bx_delay2500's Time L / Feedback L, Smack Attack's Attack / Sustain, G8's Threshold / Reduction / Attack / Release,
  DeEsser's Threshold / Freq, C6's Bands 2–5, Melda's four band thresholds and Globals → Compression.
- **Dropped where the name was wrong**: SBC's "Gain" (a path: 19.6 at −40, 13.1 at −60), bx_delay2500's "Modulation Mix" (no
  dry), H-Reverb's "Predelay Free" (12.5 → 12.5 ms), TransX's "Release" (0.00 dB), Melda's gate and processor-2 thresholds (cut
  nothing), C6's Bands 1 and 6 (cut nothing at their defaults), bx_digital's "2" bands (flat: the Side channel), SBC's Attack
  on the first run (a bound at one end — now a bound counts as its bound, confirmed 6.4 → 48 ms).
- **Measured role, unnamed**: TransX's **Range** (the transient moves 17 dB alone — the run-2 miss, found), bx_delay2500's
  **Time R** and **Feedback R** (the right channel's pair), H-Reverb's **XGain** (RT60 ×4) and **ER/Tail Balance** (the onset
  11 → 47 ms), G8's Dry/Wet (behaves as a range on a gate), J37's Input Level (drives the stage: a drive by measurement),
  J37's WOW Depth (frequency modulation leaks the fundamental into the harmonic bins — the measurement's limit, said).

## Waiting on Kathy

- The digest (`docs/PHASE_B_DIGEST.md`): when, and whether the "unnamed" lists go with it or stay internal.
- Three verdicts that are honest but awkward: J37's WOW Depth and Flutter Depth read as "drive" (modulation smears the
  fundamental across bins); a filter in a delay's feedback path reads as a "time" control (it moves where the first repeat
  clears the floor). The signatures could add a tone-share guard (the fundamental's bin must keep ≥ 90 % of the output) — a
  ruling on whether to tighten or leave them said.
- The Artist row on Sean's Mac still needs `--retry-refused` or a plan-version bump (unchanged from run 2).

## Waiting on Sean

- Everything in the digest's question lists; first among them the sustain definition and the de-esser frequency figure.

## Blocked / not done

- Timing and multiband do not probe the unnamed pool (a burst pair per control ~12 s; the multiband's pairing response IS its
  measurement step); frequency / Q signatures are not probed for unnamed EQ controls (they need a boosted band).
- The reverb tail scaling has no live unit yet (H-Reverb's labels top out at 2 s, Valhalla's are norms); the pin covers it.
- Saturn 2: the remedy (a text-pass timeout from the parameter count, or sampling only nominated controls) is not built.
- G8's Hold at '0 ms' gives no trace (the first position) — "not measured at both positions" is correct and unfixed.

## What I'd do next

1. The tone-share guard for drive and the "first repeat moved by a filter" case for time, once ruled.
2. A probe pass that samples text only for nominated controls (Saturn 2 and every 900-parameter unit).
3. The sustain definition with Sean, then the hit shape that reads it.
