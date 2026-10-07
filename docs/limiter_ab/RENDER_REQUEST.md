# Limiter v2 — renders needed from Sean (7 Oct 2026, session L)

The harness (`tools/limiter_ab_guard`) compares WAV renders. It hosts nothing. Every number it produces comes from
a pair of files: the unprocessed source and the same source through Pro-L 2. Our own v2 renders are produced
offline from the source files by `limiter_v2_render`, so the ONLY thing Pro-L 2 has to do is render each source
once. The old EchoJay Limiter render is optional (a baseline for the record).

## Folder and naming — these are exact, the harness parses the names

    ~/echojay-limiter/docs/limiter_ab/renders/
        source_<case>.wav      the unprocessed material (the five synthetic ones are already there)
        proL2_<case>.wav       the same file through Pro-L 2, settings below
        echojay_<case>.wav     optional: the shipping EchoJay Limiter, same gain and ceiling (fullmix only is enough)

Tags carry no underscore; everything after the first underscore is the case name. WAVs in that folder are
git-ignored, so size is not a concern.

## Pro-L 2 settings for EVERY render — one preset, never touched between cases

    preset             Default Setting
    style              Transparent
    gain               +8.2 dB
    output level       0.0 dB   (this is the ceiling the harness checks overs against)
    true peak limiting ON        (confirmed by Sean)
    oversampling       Off
    everything else    as the Default Setting preset leaves it

Please also write down (or screenshot) the full panel once: lookahead, attack, release, channel linking
(transient % and release %), unity gain, DC offset filter, dither (must be off). Without these the comparison is
not reproducible next month.

## Render rules (each one is a thing that has silently ruined an A/B before)

1. **Same session, same bounce range** for the source and its Pro-L 2 render: same start bar, same length, to the
   sample. The harness aligns by cross-correlation and REFUSES a pair whose offset is not constant through the file.
2. **48 kHz, 32-bit float WAV, no dither, no normalisation.** If the session is 44.1 kHz, say so and I regenerate the
   synthetic sources at 44.1 kHz (do not resample them). 24-bit is acceptable; float is better because a +8.2 dB
   source can exceed 0 dBFS inside the DAW and float keeps it.
3. **Pro-L 2 is the only thing on the path.** No other plugin, no channel EQ, no fader (0.0 dB), no pan law surprises
   (stereo file on a stereo track). Bounce the track output, not the master.
4. **Source = exactly what Pro-L 2 heard**, i.e. the signal at Pro-L 2's input with Pro-L 2 bypassed (or removed),
   bounced over the same range. For the synthetic cases that is the file I gave you, so bounce it anyway, through
   the same track with the plugin bypassed — it proves the path is unity.
5. Offline or real-time bounce: either, as long as 1-4 hold.

## The cases

### Real material (yours)

| case | what | length |
|---|---|---|
| `fullmix` | the mix-bus passage you A/B'd on 7 Oct, the one where Pro-L 2 sounded excellent and ours was bypassed. Include the loudest chorus. This is the blind-listening material. | 30-60 s |
| `kick_bursts` | kick (or drum bus) alone: isolated hits with space between them, 8-16 hits | 10-20 s |
| `bass_sustain` | the sustained bass (with kick if that is where the pumping was) — the passage that pumped | 15-30 s |

Three sources, three Pro-L 2 renders, plus `echojay_fullmix.wav` if you want the old limiter on record
(EchoJay Limiter: input_db +8.2, ceiling 0.0, transparent, TRUE PK on, lookahead and release at their defaults).

### Synthetic (already generated in the folder — only the Pro-L 2 pass is needed)

| case | what it measures | length |
|---|---|---|
| `tone_997` | THD at 0, +3, +6, +8.2 dB over; attack/release step response at each level step | 25 s |
| `tone_50` | the same at 50 Hz: does it hold a gain or ride the waveform (LF distortion), LF release | 25 s |
| `tone_imd` | 19 + 20 kHz pair at +6 and +8.2 dB over: intermodulation products | 13 s |
| `probe_transients` | 1 kHz bed at -14 dBFS with bursts of 1 sample to 1 s, at +8.2 and +4 dB over: lookahead length, attack window shape, hold, release limbs vs burst length | 37 s |
| `panned_transient` | bursts hard LEFT over a bed on both channels: channel linking (the right channel's dip) | 20 s |

Each starts and ends with 0.4 s of low-level noise; that is the alignment marker, leave it in.

## That is 8 Pro-L 2 renders and 3 source bounces

    proL2_fullmix.wav   proL2_kick_bursts.wav   proL2_bass_sustain.wav
    proL2_tone_997.wav  proL2_tone_50.wav       proL2_tone_imd.wav
    proL2_probe_transients.wav                  proL2_panned_transient.wav
    source_fullmix.wav  source_kick_bursts.wav  source_bass_sustain.wav     (+ echojay_fullmix.wav, optional)

Then: `./build-limiter-ab/limiter_ab_guard analyse docs/limiter_ab/renders` prints the full table; I run it and
keep the output under `docs/limiter_ab/results/`.

## Still to rule on (plan §4 stopping rule)

The harness implements the proposed rule as PASS/FAIL lines per case (level within 0.1 LU, zero overs, retention
within 1 dB per hit, release limbs within 10 %, THD no harmonic more than 3 dB worse, pumping no worse than
+0.2 dB). The blind listen on `fullmix` at +8.2 dB is the real gate; say if any threshold should move.
