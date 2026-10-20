# The 172 "couldn't be identified" and the 333 "won't load on this Mac" (20 Oct run). Report only, no fixes

From the backup `~/Desktop/WIZARD_RUN_20oct_DONE` (wizard_state.json, wizard.log), joined to `FOR_SEAN_20oct/trust_levels.json`. Nothing was loaded.
- **"Ours"** = a gap or bug in EJ Map.
- **"Genuine"** = the plugin can't run here, or has no EchoJay category.
- **"Possible gain"** is a ceiling. None of these rows is in today's 726 testable, except 10 of the "no signature" group, which already count as unsure.

## Won't load on this Mac: 333

| group | count | ours or genuine | possible gain |
|---|---|---|---|
| **VST3 with no Apple-silicon code (Intel-only)** | 326 | **genuine** on this Mac: no arm64 DAW can load them natively | **0 natively.** A Rosetta lane (an Intel probe run under Rosetta) could add up to 326 testable VST3 rows. 99 of them have an AU twin here (21 certified, 25 mapped, 53 unsure), so the format bridge could credit up to +21 certified / +25 mapped once they load. Cost: about 3.8 s per Intel instance. Only worth it if Rosetta DAW users matter. |
| **AU loaded but showed no parameters** | 7 | **suspect ours** for the 5 Waves (Waves Tune m/s, Waves Tune LT m/s, GTR Tuner): Waves AUs normally expose parameters, so the tree may not be populated when we read it. **Unknown** for bx_crispyscale (a scale/MIDI tool) and WS LOWEND | up to +7 testable. Tuners / pitch tools would be "pitch" if they map. |

## Couldn't be identified (sent for review): 172

| group | count | ours or genuine | possible gain |
|---|---|---|---|
| **A tail: reverb or delay** (CLA Effects, Enigma, dearVR music, EKramer FX, CLA Epic …) | 22 | **ours.** We support both categories, but the categoriser won't choose between reverb and a feedback delay, so it sends them to review | up to +22 testable, measured by the reverb / delay passes |
| **Flat at default** (Lindell 80 / 50 / 69 Bus, Orange Clip, Noveltech Vocal Enhancer, S1 Imager …) | 14 | **mostly ours:** consoles, clippers and enhancers need their drive / engage turned up before they show a signature (the same gap as the saturation engage search). S1 Imager is genuine (stereo) | about +12 testable |
| **Word-proposed: pitch** (bx_tuner) | 1 | ours (the proposal wasn't confirmed by a signature) | +1 |
| **No signature, no name clue** | 135 rows (102 names) | **split by name (my judgement, not measured):** | |
| - channel strips / mastering chains: Scheps Omni Channel, bx_control V2, bx_masterdesk ×4, AR TG Mastering ×2, CLA MixDown | ~18 rows | **ours**: channel_strip is a category | up to ~18 |
| - clippers / drive / console: StandardCLIP, bx_metal2, REDD17, MCharacter, Noveltech Character, SPL Vitalizer | ~8 | **ours**: limiter / saturation | up to ~8 |
| - filters: EMO-F2, bx_cleansweep Pro / V2, MBandPass, bx_subfilter | ~6 | **ours**: EQ (cut filters) | up to ~6 |
| - pitch / guitar: UltraPitch ×3, MFreqShifter, GTR Stomp 2 / 4 / 6 | ~10 | **ours**: pitch / amp_sim exist | up to ~10 |
| - restoration / noise: Acon DeClick / DeClip / DeHum, NS1, WNS, X-Click / Crackle / Hum / Noise, soothe2 | ~16 | **genuine**: no EchoJay category | 0 |
| - stereo / spatial: Center, Mono2Stereo, MStereo* ×4, MSpectralPan, PS22 ×3, S1 Shuffler, Schoeps Double MS, bx_stereomaker, bx_shredspread, THX, dearVR pro, bx_panEQ | ~30 | **genuine**: no category (a mono test signal can't show them) | 0 |
| - hosts, racks, multi-FX, MIDI and vocal tools: EffectRack, StudioRack, Meaw Chain, Reaktor / FM8 / Absynth MFX, SerumFX, Byome, Triad, SonoBus, MRecorder, Playhead, Pitchwheel, Melodyne, Auto-Key, MTuner, AVOX ×3, Vocal Bender, Mic Mod, Master Plan … | ~47 | **genuine**: no single category | 0 |

**Totals.**
- **Ours:** about **84 rows**: 22 tail + about 12 flat + 1 pitch + about 42 from the no-signature split + 7 no-parameters (5 of them suspect ours).
- **Genuine:** about **420 rows**: the 326 Intel-only VST3s (natively) + about 93 out-of-category tools.
- **If every "ours" fix landed:** up to about +84 testable. Mapped / certified only as their category's measuring passes allow; I'm not assuming a rate.

**Where I'd start (your call):**
1. The tail split (22, one rule).
2. The flat-at-default engage search (shared with the saturation engage gap from the 18 Oct plan).
3. The 5 Waves AUs with no parameters (one diagnosis).
