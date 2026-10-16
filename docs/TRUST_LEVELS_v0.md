# Trust levels for plugin maps - v0 draft (Kathy, 16 Oct 2026)

Status: DRAFT for Sean (server) and A (EchoJay) to agree. No code. Nothing sent.

**Product rule (Kathy):**
- **Certified** maps are preferred by the AI.
- Maps we are confident in can still be dialled: roles identified, writes read back correctly, not held for a crash, a licence or silence.
- Anything less is not auto-dialled.

---

## 0. What exists today

- **`dialable`** (derived at serve, `api/params/maps.js` / `lib/params-v2.js mapClearsCategoryBar`) is the only gate EchoJay reads.
  - It is a presence check: the category's named controls exist, or at least 2 usable named controls do.
  - Run over the 1,123 maps on Kathy's Mac with the server's own function, **all 1,123 are `dialable: true`**.
  - So `dialable` cannot separate the levels. It says "there is something to write", not "we know what it does".
- **Roles.** Real role identification exists in four places:
  - Tier 1 `params` marked `trust: "human-verified"` (34 maps);
  - EQ roled band groups (7 groups on 2 maps);
  - the measured compressor profiles (`ej_comp_profile/1`: amount, ratio, engage, neutral and make-up, by measurement);
  - the server's `compressor-profiles.json` role table (74 products; its header: "a profile is not a certification").
- **Read-back.** Three kinds exist:
  - `evidence.readback {asked, wrote, read, match}` per role (33 maps). The server stores it at `plugin:<fp>:evidence:ejmap` but does **not serve** it.
  - The compressor tone check: the profile's writes played, GR measured.
  - The wizard's read-back check ("N write(s) read back as the profile says").

  `trust: "setread"` on every control means the anchors were captured by set-then-read. It is how a control was captured, not a check of a dial write.
- **Holds.** All hold reasons are EJ Map-local:
  - `quarantine.json`: crash_on_load, hang, died_during_load;
  - cert outcomes: needs_licence, refused, needs_review, multiband, surround;
  - wizard row states: needs_licence, needs_hardware, silent, crashed, wont_load, demo, custom_default.

  The server does not receive them, except declines and `probed` verdicts.

## 1. The three levels

A plugin is judged per identity (`format|uid|version`) and per map fp. A level always comes with its reason, the "why".

### CERTIFIED

The plugin's category has a certification rule, and this identity's profile passes it.
- **Compressors (the only rule today: v1's):**
  - an `ej_comp_profile/1` for this exact identity;
  - tone check spec v1.7 PASS within 0.5 dB at g 2, 4, 5 and 6;
  - frequency, material and combined records present;
  - no frequency flag and no output-level flag;
  - a combined miss only from timing positions v1 never writes;
  - not held by Kathy;
  - and the server's own gate passes: `quality.point_error_db <= 0.5`.
- **Tuners:** the server's `tuner-profiles` `state: "certified"` ("Sean's word").
- **Every other category:** none yet. Each needs its v1 spec and its own pass test before anything can be certified.

### MAPPED (confident: may be dialled, with care)

All four must hold for the controls the dial will write:
1. **The same plugin.** A map for this exact identity, with its fp matching the loaded plugin.
   - Not served from another version: `anchors_unverified` / `served_from` means unsure.
2. **Roles identified** for each control the dial writes, by one of:
   - a Tier 1 param with `trust: "human-verified"`;
   - an EQ roled group;
   - a measured profile whose tone check passed at g 2, even if the profile is held for an accuracy flag;
   - EJ Map's measured role evidence (cert roles).

   A name match alone ("Threshold" in the control list) is NOT a role.
3. **Writes read back** for those controls, by one of:
   - `evidence.readback` with `match: true` for every role written, none false;
   - the tone check's own writes (they played and the GR landed);
   - the wizard's read-back pass for this version (rule A or B).
4. **Not held.**
   - Not quarantined.
   - Its latest outcome is not needs_licence, needs_hardware, silent, crashed, wont_load, demo, refused, needs_review or custom_default.
   - No decline on any control the dial writes.

### UNSURE (not auto-dialled)

Everything else. That includes every map that only clears today's `dialable` bar.

### How many land in each level today

These are the 1,123 maps on Kathy's Mac, judged by product name against Sean's cert 5 folder (13 Oct) and EJMAP_PUBLISH_COMPRESSORS_v3 (47 products).
- One product can have several maps (versions), so certified is 51 maps for 47 products.
- Sean's Mac and the server will differ; the server should compute these numbers itself.

| category | maps | certified | mapped | unsure |
|---|---|---|---|---|
| compressor | 222 | 51 | 42 | 129 |
| eq | 228 | 0 | 2 | 226 |
| saturation | 152 | 0 | 6 | 146 |
| channel_strip | 125 | 0 | 1 | 124 |
| reverb | 114 | 0 | 1 | 113 |
| amp_sim | 101 | 0 | 1 | 100 |
| delay | 60 | 0 | 1 | 59 |
| limiter | 46 | 0 | 3 | 43 |
| de-esser | 29 | 0 | 0 | 29 |
| transient_shaper | 23 | 0 | 2 | 21 |
| pitch | 15 | 0 | 0 | 15 |
| gate | 8 | 0 | 0 | 8 |
| **total** | **1,123** | **51** | **59** | **1,013** |

Notes on the table:
- **Compressor "mapped" (42):**
  - most are measured profiles that passed the tone check but are held for an accuracy flag: Kathy's 8, plus the re-check units that pass at g 2;
  - the rest are maps with every read-back true.
- **Other categories:** "mapped" is the handful of maps with Tier 1 roles and every read-back true.
- **Most maps are "unsure"** because roles and read-back were recorded on only 33 maps. The mapping sweep records names, not roles.
- **The biggest lever is to record read-back per role in the normal mapping sweep**, and to serve it. That moves maps from unsure to mapped without certification.

## 2. What the server sends per plugin

Add one block to each served map. Keep `dialable` for older clients, but redefine it as `level != "unsure"`.

```json
"trust": {
  "level": "certified" | "mapped" | "unsure",
  "why": "tone check v1.7 pass at 2/4/5/6 dB; no accuracy flag",   // one plain sentence, from the rule that decided
  "rule": "comp_v1" | "mapped_v0" | null,
  "identity": "AudioUnit|61126174|1.4.10",                         // the identity the level is for; another version = unsure
  "checked": { "build": "b287aef3", "at": "2026-10-12" },
  "roles": [ { "semantic": "threshold_db", "control": "Threshold", "index": 3, "source": "profile|tier1|eq_group|cert_roles", "readback": true } ],
  "held": null | { "reason": "needs_licence", "stage": "cert", "at": "..." },
  "limits": { "max_gr_db": 6, "max_gain_db": 3, "max_mix_pct": 50 }   // mapped only: the cautious envelope (section 3)
}
```

- **certified** also carries `comp_profile` (the existing `attachCompProfiles`, today behind `COMP_PROFILES`, OFF).
- **The server must start receiving and keeping:**
  - EJ Map's and the wizard's hold states;
  - the per-role read-back, already stored, now summarised into `roles[].readback`;
  - Kathy's publish holds.
- **Unknown or missing means unsure.** The client treats an absent `trust` block as unsure (today an absent `dialable` already means not dialable).

## 3. What EchoJay (A) does at each level

### Certified
Dial as today: the profile's picked writes and its make-up, preferred over any mapped alternative for the same job.

### Mapped
Dial only the controls listed in `trust.roles`, inside `trust.limits`.
- **The wording.**
  - **The card** (one line, plain):
    > Aiming for about 4 dB of gain reduction - not verified on this plugin yet, check its meter.

    Per category: "Aiming for about +2 dB at 3 kHz - not verified yet, listen and adjust"; "Aiming for about 30% wet - not verified yet".
  - **The model** gets the level and the why, so it never claims a result it has not measured. The card / model split stays as it is: the card stays quiet, the model gets the facts.
- **Cautious limits** (proposed defaults, for A and Sean to agree):
  - compressor GR at most 6 dB;
  - EQ at most ±3 dB per band;
  - mix and wet at most 50%;
  - saturation drive at most the first third of its range.
  - Move from the current setting, never jump to an end stop.
  - Never write a control outside `roles`.
- **Output kept steady.**
  - With a read-back output, make-up or trim role: hold the plugin's output level where it was (make-up = the expected GR).
  - Without one: write nothing for level. EchoJay's own pre-chain gain is not used for this (it stays user-owned); the self-check below reports the change instead.
- **The self-check** (EchoJay already measures each slot's input and output, `SlotLevels in/out`, LevelTally):
  - Take the slot's in/out levels for a few seconds of playback before the dial, then the same after.
  - Report the real result, e.g. "Measured: about 3.5 dB less on the loud parts, output level 0.4 dB lower".
  - For a compressor, the change in the out-minus-in level on the loud parts stands in for GR, since third-party GR meters can't be read.
  - If the result misses the target by more than its limit (1.5 dB, as in the Logic check), say so and offer to adjust or undo.
  - Never retry on its own.
  - The result can travel back to the server as evidence: a field report, never a certification.

### Unsure
Not auto-dialled. The AI may suggest a setting in words, naming the control, for the user to set by hand ("Try Threshold around -20 dB"). It never writes it.

## 4. Questions for Sean and A to agree

**Sean (server):**
1. Should the server compute `trust` at serve time (like `dialable`) or store it, and from which records? Proposed: serve-time, from stored evidence, declines, holds and `comp_profile`.
2. Should hold states (quarantine, needs_licence, silent, crashed, demo...) come from EJ Map's `--zip` hand-over, the wizard upload, or both? Which wins when they disagree?
3. Should `dialable` be redefined as `level != unsure` (which today turns most auto-dials off), or kept and `trust.level` added beside it for a transition?
4. Is a held profile (passed the tone check, held for an accuracy flag) a "mapped" source of roles and read-back? Proposed: yes. Its writes landed; only its accuracy is unproven.
5. Should a different version of the same product ever be "mapped"? Proposed: only after the wizard's read-back check passes for that version.
6. Is turning `COMP_PROFILES` on for v3's 47 the first step?

**A (EchoJay):**
1. Are the in/out level snapshots good enough for the self-check? Which window (seconds of playback, loud-part percentile)? And can EchoJay hold the dial until it has both readings?
2. The limits: are 6 dB GR, ±3 dB EQ, 50% mix and the first third of drive right for "mapped"?
3. The card wording: one line per dial, as above. Does it fit the 9 Aug silence rule, or does "not verified yet" count as noise?
4. With no output role, should EchoJay compensate the level itself, or only report it?
5. Should the self-check result be sent back as evidence (a field report per identity), and with what consent?

**Kathy:**
1. Is "mapped" allowed for every category from the start, or compressors and EQ first?
2. Should an unsure plugin still get a written suggestion, or nothing at all?

---

## 6. Sorted, 16 Oct (Kathy: "SORT ALL THREE"). Built and pinned; nothing sent; no server change

Code:
- `EjmapTrust.h`: the role rule, the read-back judge and credit, and the level.
- `EjmapCertV1.h`: the v1 rules.
- `EjmapTrustRun.h`: `ejmap --trust-levels` and `ejmap --cert-v1`.
- The sweep's read-back in `MainComponent.h submitMap`.

Pins: WZ-TRUST-1/2/3, WZ-CERTV1-1, WZ-SWEEP-RB-1; 21 mutants red.

### 6.1 Read-back
- **From existing data (the credit).** Every display reading the measuring runs already took at a norm they wrote is judged against **this map's own anchors**. The tolerance is one display step or 0.5% of the span, capped at 1%.
  - **Sources:** the certification fixtures (`controls[].displayAt`: every control at 3+ norms in one load), every Phase B record and draft position (`{norm, display}` under the control that owns it), and the cert profiles' amount curve.
  - **Read back** = 2 or more judged readings and no failure. One failure is a failed read-back: said, and kept on the control.
  - **Where it came from:** Sean's cert 5 and Kathy's 15 Oct night run. **9,270 controls** credited; 432 of them have a failed reading.
- **The rest (the sweep).** The mapping sweep's submit, in its one load and inside its state snapshot:
  - each anchored control is written at 25 / 50 / 75 % of its anchors, its own display read and judged, and the control put back before the next;
  - mode controls are written at two labels.
- **Saved and sent.** The result is saved on the control as `readback {checked, failed, pass, worst, source}` and goes in the map upload.
- **Credited copies.** The credit does not write the map store, which is a standing rule. The credited copies are in `~/Desktop/EJMAP_TRUST_16oct/maps`.

### 6.2 The role rule (Kathy's ruling, 16 Oct)
Name + unit identifies the role:

| role | name words | unit the display shows |
|---|---|---|
| threshold | threshold, thresh | dB |
| ratio | ratio (a word start) | x:1 (a numeric ratio from 1) |
| attack / release | attack, atk / release, recovery | ms or s |
| makeup / output / gain | makeup / output, out gain, master gain / gain | dB |
| EQ freq / gain / Q | freq, frequency / gain / Q, bandwidth | Hz (or labels in Hz) / dB / Q value |
| mix | mix, dry/wet, wet | % |
| delay time | delay time, time, delay | ms, s or a note value |
| decay | decay, RT, RT60 | s (or ms) |
| pre-delay | pre-delay, predelay | ms |
| drive | drive, saturation | % |
| ceiling | ceiling | dB |

- **Not identified:**
  - the wrong unit;
  - two candidates for the role a dial needs (Comp Threshold + Gate Threshold);
  - a control measurement refuted (`dropped`).
- **Identified by measurement**, whatever the name says:
  - a confirmed `roles_by_measurement`;
  - a draft's chosen control where that part measured cleanly;
  - a compressor profile's amount control.
- **The dial targets per category:**
  - compressor: threshold;
  - limiter: ceiling or threshold;
  - EQ: a band's freq + gain;
  - de-esser: threshold or freq;
  - gate: threshold;
  - saturation: drive;
  - amp sim: drive or gain;
  - reverb: decay, mix or pre-delay;
  - delay: time or mix;
  - transient shaper: attack, release or mix;
  - channel strip: threshold, or an EQ band;
  - pitch: none (the tuner spec decides).

### 6.3 v1 certification, the measurable parts (drafts on disk, nothing loaded): `ejmap --cert-v1`
Rules are in `EjmapCertV1.h`'s header. A product is certified for the parts that pass; a failed part (an EQ band, a delay's mix) is never offered.

| category | the test | drafts | pass | the "feel" that waits for the listening session |
|---|---|---|---|---|
| EQ | gain vs dB label within 1.0 dB (to ±12); freq vs Hz label within 1/6 oct; Q vs Q label within 25% | 149 | 9 | feel words (warmer, air, less boxy), default band per region |
| limiter | holds BS.1770 true peak, overshoot ≤ 0.1 dB | 39 | 30 | none (the LUFS target is EchoJay's) |
| delay | time vs ms / s label within max(1 ms, 2%); feedback rises; mix law ≤ 1 dB; no acceptance failure | 33 | 1 | slap / echo / ambience bands |
| reverb | RT60 rises with decay; pre-delay within max(2 ms, 10%); an acceptance step ran and passed | 92 | 27 | room-size / brightness words |
| saturation | THD rises with drive over ≥ 6 dB; an acceptance step ran and passed | 153 | 56 | subtle / warm / crunch / fuzz as THD bands |
| gate | open level vs dB label within 1.5 dB, or the acceptance steps | 8 | 5 | tight / natural; bleed / hit levels from A |

Why the counts are low:
- The drafts were measured for coverage, not for these tests.
  - 88 EQ drafts have no band that measured as static.
  - Most delay "time" displays carry no time unit.
  - 77 saturation drafts ran no acceptance step.
- Measured misses (EQ gain off by more than 1 dB at ±12, a spring's reversed decay) are real.

### 6.4 Today, before → after (the 1,123 maps on Kathy's Mac)
- "Before" is section 1's count.
- "After" uses the credit, the rule and v1.
- `~/Desktop/EJMAP_TRUST_16oct/trust_levels.json` gives each map's level and why.

| category | before C / M / U | after C / M / U | unsure because: held / role not identified / no read-back yet / read-back failed |
|---|---|---|---|
| eq | 0 / 2 / 226 | 12 / 37 / 179 | 85 / 78 / 2 / 14 |
| compressor | 51 / 42 / 129 | 51 / 32 / 139 | 112 / 20 / 5 / 2 |
| saturation | 0 / 6 / 146 | 40 / 33 / 79 | 43 / 28 / 8 / 0 |
| channel_strip | 0 / 1 / 124 | 0 / 0 / 125 | 30 / 80 / 15 / 0 |
| reverb | 0 / 1 / 113 | 33 / 21 / 60 | 22 / 28 / 7 / 3 |
| amp_sim | 0 / 1 / 100 | 15 / 3 / 83 | 59 / 24 / 0 / 0 |
| delay | 0 / 1 / 59 | 1 / 5 / 54 | 18 / 36 / 0 / 0 |
| limiter | 0 / 3 / 43 | 25 / 4 / 17 | 7 / 7 / 0 / 3 |
| de-esser | 0 / 0 / 29 | 0 / 2 / 27 | 4 / 5 / 18 / 0 |
| transient_shaper | 0 / 2 / 21 | 0 / 0 / 23 | 4 / 17 / 2 / 0 |
| gate | 0 / 0 / 8 | 4 / 0 / 4 | 0 / 2 / 0 / 2 |
| pitch | 0 / 0 / 15 | 0 / 0 / 15 | 7 / 8 / 0 / 0 |
| **total** | **51 / 59 / 1,013** | **181 / 137 / 805** | **391 / 333 / 57 / 24** |

- Compressor "mapped" fell from 42 to 32. The old count took any held profile with a passing tone check. The new one needs the amount control itself read back against this Mac's map, and judged readings exist for 32 of them.
- The 57 "no read-back yet" are what the sweep's new read-back adds when those plugins are next mapped (guard on).

### 6.5 What Sean's server must read (no server change was made)
1. `controls[].readback` on an uploaded map. The server stores `evidence` but never serves it; the per-control read-back must be kept on the control and served.
2. The `trust` block (section 2), computed from the same rules. Or EJ Map's `trust_levels.json` can be the input until the server computes it.
3. Readers for the v1 profiles of the other categories (`ej_eq_profile/1`, `ej_space_profile/1`, `ej_saturation_profile/1`, `ej_gate_profile/1`, the limiter ceiling block), with only `certified_parts` offered.
4. `COMP_PROFILES` on, for the compressor publish set (v3).
5. The hold states: the wizard upload's `skipped` and EJ Map's hand-over. Today none reaches the server.

## 7. Round 2, 16 Oct (Kathy: "NEXT ROUND")

- **Section-aware roles (ruling):** a role word under a section prefix (Comp / Gate / Exp / EQ / Band N / LF / LMF / MF / HMF / HF / De-ess, and Limiter) is identified within that section.
  - The dial takes its category's section: a compressor takes its own threshold or Comp's; a gate takes Gate's; a de-esser takes De-ess's.
  - Two candidates in one section, or an unknown prefix ("Sidechain", "1"), stay ambiguous.
  - In code: `EjmapTrust.h knownSection / preferredSections`.
- **Tempo-synced times (ruling):** note values (1/4, 1/8D, 1/16T) are a delay time, including on "Note" or "Sync" controls.
- **Multiband is held:** multiband profiling is not built, so a band threshold is not a compressor's dial. This took 20 compressor maps out of the review.
- **A bug the human-check test found:** `--trust-levels` read the answers file and the quarantine list out of a temporary JSON value, so both were empty. Fixed. Quarantine holds are now applied (+22 held).
- **Counts on the 1,123 maps:** certified 181, mapped 136, unsure 806.
  - Of the unsure: 413 held, 314 role not identified, 55 no read-back yet, 24 read-back failed.
  - Channel strips do not move: their controls (e.g. SSL Guitarstrip "Compressor Threshold") were saved with no unit, so sections cannot help. The human check settles them.
- **The human check:**
  - `ejmap --trust-levels ... --human <human_checks.json>` applies the answers. A yes identifies; a no refutes.
  - `--out` writes `review_items.json` (`ejmap_role_review/1`).
  - Today: 1,799 controls on 177 maps. 45 of them, on 43 maps, are "counts now": one yes moves the map to mapped.
  - The local page is `tools/ejmap/review/` (`review_server.py` + `admin-plugin-review.js`, the operator console's module shape). It is on Kathy's Desktop as `EJMAP_REVIEW`.
  - The admin version (route, store, import, formats) is in `tools/ejmap/review/admin_handover/` and on the Desktop as `ADMIN_REVIEW_HANDOVER`.
- **Pins:** WZ-TRUST-4 (sections, notes, the human check), WZ-TRUST-5 (held); 10 mutants red. `tools/ejmap/review/test_review.cjs` covers the module and the route.

## 8. Multiband v1, certified per band (16 Oct, Kathy: "MULTIBAND BUILD")

Code:
- the rule: `EjmapMultibandV1.h` (pure);
- the measuring: `EjmapMultibandV1Run.h`, `ejmap --cert-multiband-v1 --out <cert> [--only <product>]` and the run-all step `multiband_v1`;
- judged by `--cert-v1` / `--trust-levels` with the multiband draft and its v1 record matched by plugin_id.

Pins WZ-MBV1-1 / -2.

**A band passes** when all of these hold:

| part | test | data |
|---|---|---|
| identity | a threshold control the measurement paired with the band | the Phase B record (pairing) |
| range | gain reduction reaches 4 dB on the band's centre tone | the band ladder |
| direction | gain reduction moves one way with the threshold (either way) | the band ladder |
| threshold | the onset (GR 1 dB, the tone's RMS) tracks the dB display 1:1 within 3 dB, with a fixed reference allowed (Lindell MBC ≈ −21 dB, a dBu-style scale; C4 −5..−10 per band); one position alone within 3 dB outright | the ladder + the v1 grid (−54..−3 dBFS) |
| gain | the setting predicted for 2 / 3 / 4 dB at −12 dBFS, measured within 0.5 dB on the band's centre tone | **new**: v1 tone check |
| crossover | at the position the plugin opens with, the frequency where the two neighbouring bands' responses are **equal** (no crossover type assumed - Kathy, 16 Oct): each neighbour soloed (units with per-band solo), or each compressed alone with the others open and its cut scaled by its own depth (no solo); within 1/6 octave of the display; no crossover control = not applicable | **new**: v1 crossovers |

- **Make-up:** offered beside a passing band only when its level step matches the dB display step within 0.5 dB (new, v1).
- **Only passing bands are offered.** v1 never moves a crossover, ratio or per-band attack / release.
- **Sections that wait for the listening session:** none. The multiband feel ("tame the low end gently") is the server's wording, not a measurement.

### Results
- **On existing data (Sean's cert 5, 17 multibands):** 13 units have bands passing every part a draft can judge. None is certified yet: the tone check and the crossovers need the v1 measuring.
  - Drawmer 1973 fails threshold tracking (its display's gap to the real onset runs 18..29 dB).
  - The Lindells, SSL G3 and Pro-MB have unpaired bands.
  - DynOne3 has no bands identified.
- **Smoke test on this Mac (C4 (m), Waves v12, guarded, scratch only): all 4 bands certify.**
  - the tone check passes on every band (each ask within 0.3 dB), and the threshold tracks on all 4;
  - **crossovers, solo method:** 92 / 4066 / 11067 Hz against displays of 92 / 4000 / 11071 Hz - all inside 1/6 octave;
  - **isolated method, forced on the same unit** (EJ_MBV1_ISOLATED=1, a test switch): 93 / 3567 / 12051 Hz - also inside, but Mid sits at 0.165 octave, at the limit. The isolated method is the fallback for units with no solo (Drawmer 1973, SSL G3, OTT, DynOne3), and a result near the limit there deserves caution;
  - the first method tried (a complementary-crossover target) read 0.12..0.28 octave high on the same unit - that assumption was the error, not C4;
  - make-up: a −9 → +9 dB move measured 12.9..17.7 dB, so it is not offered;
  - about 2.5 min a unit for the v1 step, after the 3 min multiband mode.

### Profile format (`ej_multiband_profile/1`, v1; the v0 draft fields are kept)
```json
{ "plugin": { "name", "plugin_id", "version", "map_fp" },
  "topology": "multiband_offset" | "multiband_global",
  "bands": [ { "band": 2, "lo_hz": 106, "hi_hz": 3768, "centre_hz": 632,            // measured at the opening crossovers
               "threshold_control": "Band 2 Thresh", "threshold_index": 21,
               "in_at_gr": [ { "norm", "display", "gr_db_by_level": { "-30", "-24", "-18", "-12", "-6" } } ],
               "threshold_reference_db": -9.5,                                         // display -> dB RMS onset
               "certified_parts": [ "gain", "threshold", "crossover" ],
               "makeup": { "control": "Band 2 Gain", "index": 22 } } ],              // only when offered
  "crossovers": [ { "control", "display", "measured_hz" } ],                           // never written in v1
  "neutral": [...], "never_touch": [crossovers, ratios, attack / release] }
```
Only certified bands appear in `bands`. `certified_parts` on the unit lists "band N" (and "band N make-up").

### What the server reads (Sean)
1. The `ej_multiband_profile/1` bands. For an ask on a frequency region, pick the band whose measured `lo_hz..hi_hz` covers it (centre inside the region).
2. The threshold setting for g dB at the band's level: interpolate `in_at_gr` by level (the band's level from EchoJay) and by GR, then write that threshold alone.
3. Keep the output steady: write make-up = +g on that band only when `makeup` is offered; otherwise leave it, and EchoJay reports the level change.
4. Never write a crossover, ratio or attack / release in v1.
5. Holds as for every category (licence, UAD, APB / hardware, demo, silent). Multiband is no longer a hold once a band certifies.

### What EchoJay (A) asks and does
- **The ask** is one band. "Control the low end by 3 dB" means the band covering about 20–250 Hz, 3 dB of gain reduction. Regions: low (20–250), low-mid (250–800), mid (800–3k), presence (3k–8k), air (8k–20k).
- **A must send the track's level per region:** the pink-referenced macroBands meter is the candidate source, so the server can turn "3 dB" into a setting.
- **The self-check** is per band: the slot's out-minus-in level in that region, before and after.
- **The card:** "Low end: about 3 dB less - certified band" / "- mapped, not verified".

### Questions for Sean and A
1. **Sean:** for units with no solo, the isolated method is less precise (C4 forced: 0.165 octave at worst). Is 1/6 octave right for them, or should their crossover part stay "not tested" until a solo or a per-band output exists?
2. **Sean:** are the region edges above right, and is one band per ask the contract (or may an ask span two)?
3. **A:** can macroBands be sent per region with each request, and can EchoJay measure a slot per region for the self-check?
4. **Both:** do multiband asks stop at 4 dB of gain reduction (the v1 range test), or go deeper?

