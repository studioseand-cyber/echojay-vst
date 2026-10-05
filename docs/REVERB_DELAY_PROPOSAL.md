# Reverb and delay — PROPOSAL (roadmap 2.7), prototype 5 Oct 2026 (overnight run 2, R4)

Status: PROTOTYPE. Nothing exported, nothing published. Mode: `ejmap --cert-reverb-delay "<product>" [--kind reverb|delay]
[--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]`, writing `<cert dir>/reverbdelay/<identity>.reverbdelay.json`
(`ej_reverb_delay_prototype/0`). The kind comes from the ledger's category unless said.

## The measurement

The probe gained `--tail`: a 997 Hz sine burst (200 ms for a reverb, 50 ms for a delay, −12 dBFS peak) then 6 s of silence,
the output's RMS and peak per 1 ms window on the input's clock (the plugin's reported latency taken out, as the burst mode
does), after a settling half second; `tempo=<bpm>` gives the plugin a playhead (playing, 4/4, ppq advancing with the
render) so a tempo-synced delay has something to sync to — the sidechain experiment's playhead, made a mode option.
`EjmapReverbDelay.h` derives, pure:

- **Dry level** = the output in the first 2 ms of the burst (before any wet; a reverb with no pre-delay puts its earliest
  reflections inside it, said). **Wet level** = the 4 ms after the burst ends (reverb: the tail as it stood) or the first
  repeat's head (delay: at the onset the wet-only run measured). **Mix law** over 11 positions: the worst deviation of
  dry(m)/dry(0) and wet(m)/wet(1) from linear (1−m, m) and from equal power (cos, sin), fit within 1 dB, else `other`.
- **Decay**: the tail's 10 ms-smoothed envelope after the burst; T20 (−5..−25 dB) and T30 (−5..−35) by least squares,
  RT60 extrapolated; a tail that has not fallen 25 dB by the end of the window is fitted over what it fell and said to be
  longer than the window. The floor is measured (the tail's quietest tenth), never assumed.
- **Onset** = the first window 20 dB above the floor from the burst's start, in a wet-only run: pre-delay or delay time.
  **Repeats** = rising edges of the peak envelope (12 dB over the preceding 10 ms, the level the block's peak over one burst
  length), spacing and fall per repeat as medians; feedback compared with 20 log10 (label %) where the label is a %; the
  repeats' own −60 dB time from spacing × 60 / fall.
- **Tempo sync**: sync on (the switch position whose text says Host / On), tempo 90 / 120 / 140, the note control as
  instantiated; measured spacing against 60000 / bpm × beats (1/4 = 1 beat; D ×1.5, T ×2/3).

Pins D1–D7, M1–M4, L1–L3 (RoundTripTest). Every control is picked BY NAME (mix / dry-wet / blend; delay / time / pre-delay;
decay / reverb time / size; feedback / regen; sync; a note control by its "1/8"-style texts) — the weak part, see below.

## Measured on this Mac (5 Oct; `cert-traces/2026-10-05-reverb-delay/`)

| product | mix law | time control vs label | decay / feedback | tempo sync |
|---|---|---|---|---|
| H-Delay (s) | **equal power** (0.00 dB off) | Delay Sec 1 / 251 / 501 ms within 2 ms; **1540 → 1477 ms (−4 %), 3500 → 2803 ms** (the 6 s window holds one gap; re-measure with a longer tail before calling it) | Feedback 50 → −5.95 dB/repeat (3.8 s to −60), 100 → −0.67 dB, **150 / 200 → +0.2 / +0.8 dB per repeat: the repeats GROW** (feedback over unity) | 1/8D at 90 / 120 / 140 = 500.0 / 375.0 / 322.0 ms vs 500.0 / 375.0 / 321.4 expected — **exact** |
| MannyM Delay (s) | **linear** (0.04 dB off) | Delay Left ms moved nothing: 500 ms at every position (the "Delay Left Tempo" note control is engaged with no switch to turn it off by name) | −10.44 dB per repeat at the default | — |
| bx_delay2500 | unknown — the name rule took "Modulation Mix" (no dry at 0) | Time L 15 / 54 / 194 / 696 / 2500 ms all within 2 % (2475 at 2500: −1 %) | Feedback 25 / 50 / 75 % → −10.45 / −4.50 / −0.99 dB per repeat vs 20 log10 −12.04 / −6.02 / −2.50: **~1.5 dB less fall than a linear-amplitude law at every position** (75 % behaves as 89 %); 100 % → 0.01 dB (unity, holds) | no sync switch found |
| CLA EchoSphere (s) | other (Mix is not the slap's dry/wet: wet at 1 = −43 dB) | SlapTime labels 10224 / 20448 … (not ms; the slap read 152 ms at every one — not the slap time) | SlapFbK 75 → −2.5 dB/repeat, 100 → 0.00 (unity) | — |
| ValhallaVintageVerb | **equal power** (0.00 dB off) | PreDelay labels are norms (0 … 1): onset 5.5 / 25.5 / 105.5 / 262.5 / 505.5 ms — a curved taper to 500 ms | Decay labels are norms: RT60 1.22 / 1.47 / 9.02 s at 0 / 0.25 / 0.5; 0.75 and 1 longer than the 6 s window | — |
| H-Reverb (s) | other (linear 12.8 / equal power 8.9 dB off: a send-style law) | "Predelay Free" moved nothing (12.5 ms at every position: the sync'd predelay is engaged) | the name rule took "Buildup Time" (not the decay): RT60 2.5–3.7 s, 8.7 s at 2.000 | — |
| Abbey Road Plates (s) | other (equal power within 2.77 dB) | **Predelay 0 / 125 / 250 / 375 / 500 ms exact to 0.5 ms** | no decay control by name (a plate: "Damper"?) ; RT60 1.34 s (T20) at the default | — |
| bx_rooMS | — | **the probe crashed (signal 11) in `--text-at all`** | | |

What the numbers say:
- **The mix law differs by vendor and by product**: H-Delay and Valhalla equal power to 0.00 dB; MannyM linear to 0.04; H-Reverb a
  send law (dry never falls like either); a "touch of reverb" at 20 % is −1.9 dB of dry on one and 0 dB on another. The law belongs
  in the profile, per product.
- **Time labels hold to 2 % on the units with ms labels** (bx_delay2500, Abbey Road Plates, H-Delay under 1.5 s). Labels that are
  norms (Valhalla) or codes (EchoSphere) need the measured map, not a comparison.
- **Feedback % is not amplitude %** on bx_delay2500 (−1.5 dB per repeat less fall than 20 log10 at every position); H-Delay's
  feedback over 100 grows the repeats. "Feedback 50 %" is a different decay on each.
- **The probe's playhead works**: H-Delay syncs to it exactly at three tempos.
- **The name rule is the weak part again**: three of eight picks were wrong or missing (Modulation Mix, Buildup Time, a sync'd note
  control with no switch). The measurement can decide instead: the control whose positions change the onset is the time control,
  the one that changes the fall per repeat is feedback, the one that moves dry and wet in opposite directions is the mix.
- **The window is too short for long reverbs**: 6 s reads RT60 up to ~8 s with a T20; Valhalla at 0.75+ needs 15 s or more.
  A longer `tail_s` for the decay runs is a one-line change; the cost is render time.

## Proposed profile fields (for Sean, after the ruling on what the server wants)

`mix: { law, worst_db, points[] }`, `time: { control, map[{norm, display, ms}] }`, `decay: { control, map[{norm, display, rt60_s}] }`
(reverb), `feedback: { control, map[{norm, display, db_per_repeat, seconds_to_minus_60}] }` and `sync: { control, on_norm,
tempos[{bpm, ms, beats}] }` (delay), each measured on the unit at its instantiate state, said.

## Questions for Sean

1. Does the server write mix as a dry/wet percentage, or as a target wet level in dB below dry? (The law decides what "20 %" means.)
2. Reverb decay: RT60 from a 997 Hz burst (a band figure) — or a broadband burst? The T20 figures here are at 1 kHz.
3. Delays with a note control and no sync switch (MannyM): should the profile carry the note values as the time map?
