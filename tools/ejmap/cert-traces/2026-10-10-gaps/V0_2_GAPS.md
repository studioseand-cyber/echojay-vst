# SPEC v0.2 rulings vs EJ Map, 10 Oct (against ~/Desktop/SPEC_RULINGS_v0_2.md)

Each ruling checked in the source. "Not EJ Map" = server, card, plugin, listening session or [A] (EchoJay sends a track figure).

## Built before today (checked in the code)
| ruling | where |
|---|---|
| timing 63 % both | EjmapTiming.h kAttackFraction / kReleaseFraction 0.63 |
| timing 1 ms / 4 kHz second pass | EjmapTiming.h kFastPassHz 4000, kFastPassWinMs 1 |
| timing >0.5 dB steady-GR shift refused | shifts_amount + note "not for the server's use" (EjmapTiming.h:198, CertDriver timeNotes) |
| program-dependent: both release times | time block release_short_burst_ms / release_long_burst_ms |
| gain: standalone ej_gain_profile/1, writes by curve incl. display_off / not_db_scale, level_dependent kept as drive_curve, bar 0.2 dB | EjmapGainCal.h (writable, kAcceptDb, level_matching / drive_curve) |
| limiter: true peak, overshoot lowering, oversampling on for TP, ceiling block | EjmapLimiter.h (BS.1770, kLowerCapDb, settingFor, ceilingBlock) |
| EQ centre_hz / corner_hz, Q at two gains, dynamic out of scope, default_bands | EjmapEq.h (kBoostDb 6 / 3, verdict dynamic, defaultBands) |
| de-esser noise primary + tone cross-check, measured frequency, split-band mode recorded | EjmapDeesser.h (noise ladder, kToneNoiseDisagreeDb, mode block) |
| saturation five steps with THD bands, compensation always | EjmapSaturation.h steps() 0-4, output choice |
| reverb full_wet / cannot_full_wet at 40 dB, wet_gain_db, send-only = full wet | EjmapReverbDelay.h fullWetBlock, kFullWetDryDb |
| reverb decay pink (broadband) + 997 Hz cross-check; note values as the time map | CertDriver reverb/delay steps 4 and 6 |
| transient 500 ms hits + held tone; gate threshold from the ramp; 1 dB / 20 dB + full timings; expander verdict | EjmapDynamics.h hitProbeArgs, heldSustainDb, ramp, attack_ms_to_1db / _full |
| multiband: own band thresholds only, floating bands, depth reference + signed GR, offset_db / in_at_gr | EjmapMultiband.h (9-10 Oct) |
| tuner: default step 1, tune_step, key from the plugin, chromatic fallback | EjmapTunerProfile.h feel block |

## Not built - built today (pinned, mutant red, rehearsed)
| # | ruling | commit | pin | rehearsal |
|---|---|---|---|---|
| 1 | delays' own card steps (-30 default, -26 cap) | bd35656b | RD10 | ADA STD-1 record: default / cap written (null: span -10.5..+0.8 dB) - delay_steps_rehearsal.txt |
| 2 | limiter = ONE profile (compressor profile + ceiling) | 5185311f | LIM-ONE | --phaseb-drafts on Sean's folder: 39 / 39 limiters the block alone (no limiter has a compressor profile) |

## Not built - held for Kathy
- **The compressor half for limiters** (gap 2's other half): no limiter runs through the compressor certification, so none has
  the GR-predicting profile the ruling makes "the profile's job". A new run step (limiters through --cert, then the tone check);
  it adds night hours. Needs a ruling.
- **Draft status lines say "v0.1"** (drafts::statusLine: "DRAFT against <SPEC> v0.1 (a proposal)") while every spec tag says
  "v0.2 PROPOSAL". One-line fix, but it changes every draft's status field: held so today's equality stays readable.

## Not EJ Map
attack_step / release_step / tune_step / sat_step on [CURRENT CHAIN] (plugin + server); timing / de-esser / saturation / reverb /
transient feel bands (listening session); the server picking a band or step, the card blend; Loudness as a LUFS target (EchoJay
already lands it); split band chosen automatically (server, from the recorded mode); the gate threshold from bleed / hit levels,
the de-esser 4-10 kHz RMS, the saturation peak / crest ([A]); the guitar vocabulary (later); expanders, dynamic / auto EQ (out of scope).

## Equality (the classification)
- Compressor derive-only, build tree 5185311f vs packaged 00f2ae79, Sean's current folder / 4 Oct zip / cert_sc / cert_tc35:
  ZERO differences (rows, fixtures, profiles, controls) - equality_compressor_vs_00f2ae79.txt.
- --phaseb-drafts over Sean's cert 2 + 3 (copy): 1106 drafts, 1067 identical, 39 differ - all limiter drafts, each exactly
  `compressor_profile: null` + the one v0.2 note (gap 2, EXPECTED). UNEXPLAINED 0.
- Gap 1 changes only a NEW delay run's acceptance (default / cap instead of touch..drenched); nothing derive-only sees it.
- b98788e2 (run-all notes) and the --phaseb-all --dry-run refusal change no output.
