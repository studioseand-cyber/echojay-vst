# Proposal for Sean (7 Oct 2026, no code): what the server should do about two things the accuracy passes measured

Both come from the 7 Oct build's data passes on six local profiles (A1 `combined`, A3 `frequency`), to be repeated on your
set tonight. Numbers quoted are measured here; the rule proposed is the smallest change to the server that uses them.

## 1. Units whose attack and release change the amount (A1: Lindell 7X-500 off by 2.8 dB, elysia mpressor by 2.1 dB)

**What was measured.** The profile's curve is swept at the instantiate attack and release. The time draft (timing spec v0.1)
records, per attack and release position, `gr_shift_db`: how much the GR step at the pick moved against the instantiate
position. A1 composed a full setting (pick at g = 4, engage, neutrals, ratio, the attack and release with the largest shifts,
make-up from the gain draft) and predicted GR = g + attack shift + release shift. The miss: 7X-500 predicted 10.61, measured
7.83 (−2.78); mpressor predicted 0.27, measured 2.41 (+2.14); bx_opto and Lindell SBC (shifts ≈ 0) within 0.02. **The shifts
are real but they do not add** — each was measured with the other time control at instantiate, and the two interact (a fast
attack with a slow release holds more GR than either alone).

**What this means for the server.** A profile's `in_at_gr` curve is only a prediction at the instantiate attack and release.
When the server writes a different attack or release (a brief that says "slow attack", or a genre preset), the pick it computes
from the curve is off by the shift — up to ~3 dB on these units, nothing on units whose timing does not move the amount.

**Sign convention.** `gr_shift_db` = the GR step at the position minus the GR step at the instantiate position; positive =
MORE gain reduction at that position (the 7X-500's Fast attack +3.41, Slow release +3.20; the mpressor's 150 ms attack −1.41).

**Decide part 1 only after tonight's data.** The 7 Oct build's `--redo combined` reads the attack-only and release-only
settings at the SAME scaled hold as the combined read (10× the slower time constant, never under 2.5 s) and records
`additivity_check`: if the two shifts add at that hold, the time draft's short-burst shifts were under-settled and part 1
holds; if they still do not add (here: 7X-500 +3.38 + 3.18 = 6.56 against a combined +3.83 at 2.5 s), part 1 is only a first
estimate and part 2 carries the correction.

**Proposal (three parts, smallest first).**
1. **Carry the shifts, write the pick from them.** Put the time draft's `gr_shift_db` per attack and per release position into
   the profile as `time.attack[].gr_shift_db` / `time.release[].gr_shift_db` (already measured; a schema addition only). When
   the server writes attack position A and release position R, it asks the curve for g' = g − shift(A) − shift(R) instead of g
   (one subtraction each). On a unit where the shifts are 0 nothing changes.
2. **Mark the units where that is not enough.** A1's record says, per unit, whether the additive prediction held within 0.5 dB
   at the largest-shift corner. Export that as `time.shifts_additive: true | false` with the measured miss. For `false` units
   (7X-500, mpressor here) the server should write the amount from the curve, then read back GR if it can (EchoJay's readback
   path) and correct once; if it cannot read back, keep attack and release at instantiate for that unit and say so in the
   brief's reply ("timing left at the unit's default: it changes the amount by up to 2.8 dB on this unit").
3. **Only if 2 is common on your set:** a second curve at the "Polished" timing row (spec v0.1, ~150–300 ms release) — one
   more sweep per unit, ~3 min each — so the server interpolates between two measured timings instead of adding shifts. Decide
   after tonight's A1 data: if fewer than ~10 % of units are `shifts_additive: false`, part 2 is enough.

## 2. Units that compress less at low frequencies (A3: mpressor 0.48 dB at 100 Hz against 1.99 at 997; 7X-500 3.10 at 100 Hz)

**What was measured.** The tone check's process at the pick, at 100 Hz and 5 kHz as well as 997 Hz, the same level. Four of
six units within 1 dB across the tones (bx_opto 0.00, SBC 0.21, C1 0.62, RCompressor 0.73). Two flagged `frequency_dependent`:
the mpressor reads 0.48 dB at 100 Hz (its sidechain filter: lows do not drive the detector) and the 7X-500 reads MORE on lows
(3.10 at 100 Hz: an emphasis the other way).

**What this means for the server.** The profile's curve is a 997 Hz fact. On a flagged unit, a vocal (energy 100 Hz–4 kHz,
weighted low-mid) will get less GR than the curve says when the detector ignores lows, and more when it emphasises them — the
A2 material pass showed the direction (the 7X-500 read 3–4 dB more GR than the curve on drums and mix). The server has no
way to know this from today's profile.

**Proposal.**
1. **Export the three readings.** `detector.frequency: {"100": gr, "997": gr, "5000": gr, "flag": true|false}` from the A3
   record — measured, no new sweep.
2. **Weight the pick by the material's spectrum, only on flagged units.** The server already knows the material's RMS (the
   meter transport); EchoJay's macro bands give its energy in low / mid / high. For a flagged unit, scale the asked g by the
   ratio GR_band / GR_997 weighted by the material's band energy (one multiply per band, three bands), then take the pick from
   the curve at the scaled g. Unflagged units: no change (the ratio is 1 within the bar).
3. **Say it in the reply.** When the scale moves g by more than 0.5 dB, the brief's answer names it ("this unit's detector
   ignores lows: asked for 2 dB more to land 4 dB on a bass-heavy vocal"), so a surprising readback has its reason on screen.

**What I would NOT do:** re-sweep every unit at three frequencies (3× the batch) — the three-point reading per unit is enough
to classify, and only flagged units need the weighting; nor build a per-band curve on the client — the server owns the pick.

**For tonight's data:** A1 and A3 run on every certified compressor (`--redo combined`, `--redo frequency`, ~20 + 70 min on
your Mac); the two records per unit give the counts these proposals turn on: how many units are not additive, how many are
frequency-dependent, and by how much.
