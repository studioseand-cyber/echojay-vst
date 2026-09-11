# Antares retune-speed sweep - five bounces, ONLY Retune Speed varying

Purpose: calibrate EchoJay's new single 0-400 retune dial (speed AND depth
together) against Antares by MEASUREMENT, not assumption. Without these
five bounces the curve between their 0 and their 400 is a guess.

Same vocal, same everything, five bounces. Suggested: sourceNEW (the
standing reference take) so every ruler already on file applies.

Auto-Tune settings, IDENTICAL across all five except Retune Speed:
  Input Type Alto/Tenor, Key D, Scale Minor, Detune 440, Flex-Tune 0,
  Humanize 0, Natural Vibrato 0, Targeting Ignores Vibrato ON,
  Transpose 0, Formant on, Mix 100.

The five, named exactly:
  antares_retune000.wav    Retune Speed 0
  antares_retune050.wav    Retune Speed 50
  antares_retune100.wav    Retune Speed 100
  antares_retune200.wav    Retune Speed 200
  antares_retune400.wav    Retune Speed 400

Bounce each from the same region start (so the files align sample-for-
sample with sourceNEW; a few samples of offset is fine, the ruler finds
it), 48 kHz, no other processing on the track.

AT BOUNCE TIME, per the standing provenance rule (REFERENCE_SET.md): note
beside each file name the Retune Speed shown on the Auto-Tune UI, the
Key/Scale/Detune shown, and the bounce date/time. A screenshot of the
Auto-Tune window at each setting is the cheapest proof. A row without
provenance is not a reference row.

What will be measured on each: activity (|output - source| per hop),
off-grid to D minor, improve-rate vs source, and word-start glitch events
- and then EchoJay's (retune_speed_ms, depth) pair fitted at each of the
five dial positions so all four columns match. Residuals will be
reported per position; a position that cannot be matched will be named
with the size of the miss, not smoothed over.
