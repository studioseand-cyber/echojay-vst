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
- **Every part must hold** (17 Oct): a threshold that cannot be tested (a display that is not in dB, e.g. OTT's 0-100) does not certify the band. A crossover is the one part that may be "not applicable" (no crossover control).
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
- **Overnight on this Mac (17 Oct, ad50ddbb, guarded, scratch only): 12 multibands measured, 10 certify bands.** Traces: `tools/ejmap/cert-traces/2026-10-17-overnight/`.

| unit | bands certified | why the rest are not |
|---|---|---|
| C4 (m), C4 (s) | 4 of 4 | - |
| C6 (m), C6 (s), C6-SideChain (m), C6-SideChain (s) | 4 of 6 | bands 5, 6 (the floating bands): the 2 dB ask measured 1.17 / 1.18 dB |
| LinMB (m), LinMB (s) | 4 of 5 (1, 2, 3, 5) | band 4: its ladder does not reach 4 dB at -12 dBFS |
| Lindell MBC | 2 of 3 | band 3: no threshold paired by measurement |
| Lindell 354E | 0 of 3 | band 1 unpaired; bands 2, 3: the display does not track (-20 dB sits 4.5..10.9 dB off) and the tone check misses |
| OTT | 0 of 3 | its threshold display is 0-100, not dB: "not tested", so no band certifies (band 2's 2 dB ask also measured 0.57) |
| DynOne3 | 0 | no bands identified |

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


## 9. Round 3, 17 Oct (Kathy: "NEXT"). Built and pinned; nothing sent; no server change

### The recency rule (pins WZ-TRUST-8)
- Every reading source carries its plugin version and date: fixtures `version` + `sampledAt`; Phase B rows and profiles `plugin.version` + `measured.date`; the read-back sweep `version` + `measuredAt`.
- A control's failed reading is **set aside** when a NEWER source, on the map's own version, read the control back at 2+ settings with none failing.
- An old failure **stands** when the newer source is another version, is older, is a single reading, or itself fails, and when the failure has no date.
- The credited map's `readback` carries `superseded: n`. The trust run prints the totals: on Kathy's Mac, 238 old failed readings on 45 controls.
- **For Kathy to judge:** the newer readings are at 25 / 50 / 75 % of the span, while most set-aside failures sit in the top tenth (norm 0.875..0.95), off by 1..5 %. Two are larger: H-Reverb long Buildup Time, 16 %; SSLEQ (s) HF Frq, 12 %. A tighter variant would set a failure aside only when a newer reading lands within 0.1 of its setting.

### The old map format (WZ-TRUST-9)
- Maps that keep role controls in the role-keyed `params` block (CLA-76 m/s, PuigChild 670 s) are credited and read back from those entries.
- J37 m/s has no Saturation control in its map, and SSLGChannel (s) HF Frq was declined by the mapper ("getText lied"). Read-back cannot reach either; they need a map that carries the control.

### Which map the read-back uses (WZ-RBSWEEP-2)
- The read-back uses the AudioUnit map of the installed version, else the newest AudioUnit map; never a VST3 map.
- The 16-17 Oct overnight run read Meaw Chain, TB_Sibalance_v4 and ValhallaVintageVerb against their VST3 map. The name check refused those readings; they were re-read on 17 Oct.
- **Open:** trust levels still judge a product's AU readings against its VST3 map too. ValhallaVintageVerb's VST3 map has Decay in ms while the AU display shows the bare normalised value, so it fails. Proposal: carry the format per source and judge a map only by readings in its own format.

### Review pairs and the first category (WZ-TRUST-10, -11)
- No EQ row could ever "count now": an EQ needs a band's freq AND gain, so a single yes never maps it.
- Two rows whose yeses together map the map now carry `counts_with` (each names the other). The page, the local server and the admin route count such a map once both are yes.
- `--trust-levels --review-first eq` writes `first_category` and orders that category's counts-now rows first, in the file and on the page.

### Section of "...Frequency" (WZ-TRUST-11b)
- `sectionOf` removed role words in table order, so "Mid Frequency" became "mid uency" and never paired with "Mid Gain". The words now come off longest first.
- This maps 19 more on Kathy's Mac (10 EQ, 9 strips), each by roles measurement confirmed and read back.
- With that, no EQ map is left where one or two yeses would map it.
- **Correction:** commit 6e82eb61's message names 8 EQ pairs. That came from an estimate that paired Input Gain with Mid Frequency; the rule rightly refused it. The real pairs were blocked by this section bug.

### Stepped controls (report only, Kathy 17 Oct)
- Writes "did not land" on Lindell 50 / 69 Channel, Harris Doyle Natalus DSCEQ and TB_Sibalance_v4. **This is not a licence problem.** The plugins snap a write to their own steps (0.25 -> 0.2222 on a 1/9 grid; 0.25 -> 0.2 on a 1/5 grid), and the display shows the snapped value.
- The maps describe these controls as continuous, and the 25/50/75 % targets fall between steps.
- Possible fixes: aim at the map's anchor norms nearest 25/50/75 %, or judge at the value the plugin took (getValue) rather than the one asked.

### Recount, Kathy's count rule (held out), 17 Oct

| category | certified | mapped | unsure | testable |
|---|---|---|---|---|
| eq | 12 | 57 | 75 | 144 |
| compressor | 60 | 37 | 25 | 122 |
| saturation | 40 | 33 | 36 | 109 |
| channel_strip | 0 | 15 | 80 | 95 |
| reverb | 33 | 43 | 16 | 92 |
| delay | 1 | 15 | 26 | 42 |
| amp_sim | 15 | 3 | 24 | 42 |
| limiter | 25 | 6 | 8 | 39 |
| de-esser | 0 | 15 | 10 | 25 |
| transient_shaper | 0 | 5 | 14 | 19 |
| pitch | 0 | 0 | 8 | 8 |
| gate | 4 | 0 | 4 | 8 |
| **total** | **190** | **229** | **326** | **745** |

- **Can't test on this Mac:** 378 (UAD 332, demo 28, silent 9, APB / hardware 6, licence 3).
- **Inputs:** the maps are Kathy's store with the 4 re-mapped products (AudioTrack s, ValhallaVintageVerb, Meaw Chain, TB_Sibalance_v4) swapped for their 17 Oct maps (scratch only). The cert data is Sean's cert 5, the smoke cert, the overnight cert and the wizard's 15 Oct folder, plus Kathy's answers.
- **The review list:** 877 rows on 106 maps. 16 rows (7 maps) where one yes counts; 108 rows (9 maps) in pairs.

## 10. Kathy's three rulings, 17 Oct ("START"). Built and pinned; nothing loaded, nothing sent; no server change

### 10.1 The recency reach (pin WZ-TRUST-12)
- **The choice.** Kathy offered two forms: a set-aside only within ±0.1 of the failure's setting, or offering the control only over its proven range. **Built: the ±0.1 rule.**
  - A proven range would be a new served field that nothing reads today: Sean's server does not serve it and EchoJay's dialler does not clamp to it. Until both do, a "mapped" control would still be dialled over its full range, so the label would claim more than the dial honours.
  - The ±0.1 rule is local to the judge and errs on the side of "unsure". It can be revisited when a range field exists on both sides.
- **The rule.** A failed reading is set aside only when a NEWER source, on the map's own version and format, read the control back cleanly at 2+ settings **and** one of that source's readings lands within 0.1 (normalised, either side, inclusive) of the failure's setting. A failure the newer reads did not come near stands, as does an undated one.
  - `EjmapTrust.h RECENCY_REACH`.
- **Effect on Kathy's Mac.** Set-aside readings went from 238 on 45 controls to 143 on 25.
  - 10 AudioUnit maps fall from mapped to unsure. Each now carries a top-tenth failure that the 25/50/75 % reads never came near: VComp (s/m) Input, Q1 (m) Band 1 Frq, API-550A (s/m) Low Gain, F6-RTA (s/m) Band 1 Freq, REQ 2 (s) Band6 Frq, LinEQ Lowband (s) Band2 Frq, AMEK EQ 250 HF Freq 1.
  - A read-back at the failure's own norm would settle each one (guarded, one load per product). It has not been run.

### 10.2 Stepped controls (pin WZ-RBSWEEP-3)
- **The rule.** A write the plugin snaps to its own step is judged at the value it TOOK (the probe's `getValue`), not at the value asked. Readings at one taken value are one setting: three writes that all stayed at 0.5 are one reading, never a read-back.
  - The read-back sweep keeps snapped writes as `snappedAt {taken: text}` plus `snapped_from {taken: written}`.
  - `readingsOfRecord` reads them and the credit counts them (`readback.snapped`).
  - The app's mapping sweep (MainComponent submitMap) already judged at `getValue`. It now counts each taken value once and records `readback.snapped` (EjmapSchema emits `snapped`).
- **`ejmap --readback-rederive --maps <dir> --from <cert> --out <cert>`** rebuilds the sweep records from the raw probe output kept in `<from>/phaseb/readback/raw`. Nothing is loaded. Each record keeps its reading's own `measuredAt`, and `--out` may not be `--from`.
  - The overnight cert: 116 records rebuilt, 106 controls with snapped writes, 70 controls read that were not read before.
  - Every one of the 3,337 previously read controls kept its `displayAt` byte-for-byte. Where a landed write and a snapped write took the same value, the landed write is kept in `displayAt` (Lindell 80 High Frequency).
- **Effect.**
  - Lindell 50 Channel (Low Freq + Low Gain) and Lindell 69 Channel (High Freq + High Gain) become mapped.
  - **Harris Doyle Natalus DSCEQ stays unsure, and not for a stepped reason.** Its 12 stepped controls now read back, but the gain it needs (High / Low Mid Gain) was declined by the mapper ("flat both ways - unbuildable"), so the map does not carry it. This is the same kind of gap as J37 and SSLGChannel (s).
  - TB_Sibalance_v4: its AudioUnit map stays mapped (Range). Its VST3 map falls under 10.3.

### 10.3 One format per map (pin WZ-TRUST-14)
- **The rule.** A map is judged only by readings in its own format. An AudioUnit reading neither proves nor fails a VST3 map. A source whose format is not stated proves nothing.
- **Where the format comes from.** The record's `format` or `plugin.format`, else its `plugin_id` ("AudioUnit|..."), else the file name its writer gave it ("AudioUnit_<uid>_<ver>", "eq.VST3_..."). Nothing is guessed: in particular, not "the probe hosts AudioUnits".
  - The read-back sweep now writes `format` itself.
- **Effect.** Every reading on Kathy's Mac is an AudioUnit reading, so **no VST3 map is mapped any more**.
  - 36 VST3 maps fall to unsure: 29 of them have a mapped AU map, 1 an unsure one, and 6 have no AU map in the store (EchoBoy, EchoBoy Jr, Little PrimalTap, Little Radiator, Sie-Q, Decapitator).
  - ValhallaVintageVerb's VST3 map no longer fails on AU displays; it is unread.
  - No AudioUnit map lost a level to an unstated format.
- **Open for Kathy:**
  1. 34 VST3 maps are still **certified**, because certification (and measured roles) are matched by product name across formats. The ruling as worded covers readings only, so this was left alone.
  2. The probe can host VST3 (since 22 Sep). A VST3 read-back (`--readback-sweep` planning from VST3 maps) would read these maps directly. Soundtoys, kHs and SSL stay behind the guard.

### 10.4 Recount, Kathy's count rule (held out), 17 Oct after the three rulings

| category | certified | mapped | unsure | testable |
|---|---|---|---|---|
| eq | 12 | 46 | 86 | 144 |
| compressor | 60 | 27 | 35 | 122 |
| saturation | 40 | 20 | 49 | 109 |
| channel_strip | 0 | 17 | 78 | 95 |
| reverb | 33 | 40 | 19 | 92 |
| delay | 1 | 10 | 31 | 42 |
| amp_sim | 15 | 3 | 24 | 42 |
| limiter | 25 | 5 | 9 | 39 |
| de-esser | 0 | 13 | 12 | 25 |
| transient_shaper | 0 | 4 | 15 | 19 |
| pitch | 0 | 0 | 8 | 8 |
| gate | 4 | 0 | 4 | 8 |
| **total** | **190** | **185** | **370** | **745** |

- **Can't test on this Mac:** 378 (UAD 332, demo 28, silent 9, APB / hardware 6, licence 3). Unchanged.
- **The path:** 190 / 229 / 326 → (the reach + the format) 190 / 183 / 372 → (the stepped re-derive) 190 / 185 / 370.
  - Mapped falls by 44: 10 AudioUnit maps to the reach and 36 VST3 maps to the format rule, with 2 gained from the stepped rule.
- **Inputs:** as 9's recount, with the overnight cert's sweep records replaced by their re-derived copies (`--readback-rederive`, scratch). `recount.sh` takes the cert as `CERT_OVERNIGHT`.
- **The review list:** the same 877 rows. 9 rows' `read_back` is now false, because their read-back came from the other format. No row's "a yes counts" changed.

## 11. Rulings 2, 17 Oct (Kathy). Built, pinned and run on this Mac (guarded); nothing sent; no server change

### 11.1 Certification by format (pin WZ-TRUST-15)
- **The rule.** Certification counts only for the format it was measured in.
  - Publish list: a product takes its format from the published profiles, `--trust-levels --certified-profiles <dir>` (the v3 publish zip unpacked: `~/Desktop/EJMAP_TRUST_17oct/published_v3`). A listed product with no profile stating a format certifies nothing; the run says which.
  - v1 drafts and multiband: from the draft's `plugin_id` or file name.
- **Effect.** All 47 published profiles are AudioUnit. Certified 190 → 156: the 34 VST3 maps lose it.
  - 11 of those 34 are UAD or demo, so under the count rule they now sit on the held line. Testable 745 → 734, held 378 → 389.

### 11.2 The VST3 read-back (pin WZ-RBSWEEP-4)
- **`ejmap --readback-sweep --format VST3`** plans from the VST3 map.
  - Its products come from the ledger's `scan-cache.xml` (read only), through the same guard. PACE is checked on the .vst3 bundle, and a bundle that cannot be checked is held.
  - The installed version is the one the probe's VST3 class reports. The probe now prints `\tversion\t<v>` on its `vst3 class:` line; the scan cache's version (4 Aug) is not used.
- **Run on this Mac** (scratch ledger, maps read only, output `~/Desktop/EJMAP_TRUST_17oct/rulings2_cert`).
  - 70 VST3 products: the 36 that fell under the format rule plus the 34 certified ones.
  - **The guard dropped 65**: UAD 10, SSL 13, Soundtoys 7, kHs 5, and 30 PACE-wrapped bundles (Melda, Slate VBC, Antares, Newfangled, FG-X 2 and others).
  - 5 were read (342 controls), and every installed version equals its map's.
  - Mapped: Heatwave (Drive), Quantum (Attack Volume), TB_Sibalance_v4 VST3 (Range).
  - Still unsure: Solid Bus Comp, whose threshold role is from measurement but whose map carries no Threshold control (the J37 kind of gap), and Spectre (drive role not identified).

### 11.3 The read-back at each old failure's own setting (pin WZ-TRUST-16)
- **The settings.** The credited copy keeps each standing failure's norm (`readback.failed_at`). `--readback-sweep --at <json>` reads only the listed controls, at their three targets plus those norms.
- **Run on the 10 AudioUnit maps** (all past the guard; same folder). Every failure reproduced on this Mac, at the installed version (= the map's), so all 10 stay unsure:
  - VComp (s/m) Input -21.3 dB at 0.033;
  - Q1 (m) Band 1 Frq 6411 / 8661 / 11701 / 13601 at 0.833..0.9375;
  - API-550A (s/m) Low Gain -12 / +12 dB at 0.05 / 0.95;
  - F6-RTA (s/m), REQ 2 (s) 11701 at 0.917; LinEQ Lowband (s) 11585; AMEK EQ 250 18.0 kHz at 0.875.
- These are the maps' anchors being off at the ends of the range, not stale readings. A re-map (or anchor repair) is what moves them.

### 11.4 Recount, Kathy's count rule, 17 Oct after rulings 2

| category | certified | mapped | unsure | testable |
|---|---|---|---|---|
| eq | 9 | 46 | 88 | 143 |
| compressor | 52 | 27 | 43 | 122 |
| saturation | 33 | 21 | 52 | 106 |
| channel_strip | 0 | 17 | 78 | 95 |
| reverb | 26 | 40 | 23 | 89 |
| delay | 1 | 10 | 31 | 42 |
| amp_sim | 13 | 3 | 25 | 41 |
| limiter | 19 | 5 | 12 | 36 |
| de-esser | 0 | 14 | 11 | 25 |
| transient_shaper | 0 | 5 | 14 | 19 |
| pitch | 0 | 0 | 8 | 8 |
| gate | 3 | 0 | 5 | 8 |
| **total** | **156** | **188** | **390** | **734** |

- **Can't test on this Mac:** 389 (UAD 341, demo 30, silent 9, APB / hardware 6, licence 3).
- **Reproduce:** `EXTRA_CERT=$PWD/rulings2_cert CERTIFIED_PROFILES=$PWD/published_v3/EJMAP_PUBLISH_COMPRESSORS_v3 CERT_OVERNIGHT=$PWD/overnight_cert_rederived ./recount.sh <ejmap> <out>`, run in `~/Desktop/EJMAP_TRUST_17oct`.
- **The review page** (`~/Desktop/EJMAP_REVIEW`, and the admin handover's sample) is regenerated: 912 rows on 110 maps. The 35 new rows are on 4 VST3 maps that are no longer certified (MReverbMB, MReverb, MAmp, SSL Native X-Saturator v6). The morning list is kept as `review_items.17oct-morning.json`; the answers file is untouched.

## 12. NEXT / ALL DAWs, 17 Oct (Kathy). Built, pinned, run on this Mac (guarded); nothing sent; no server change

### 12.1 Counts split by format (Kathy's count rule; trust_levels.json rows now carry `format`)

| format | certified | mapped | unsure | testable | can't test on this Mac |
|---|---|---|---|---|---|
| AudioUnit (Logic) | 156 | 194 | 261 | 611 | 197 (UAD 167, demo 14, silent 7, APB / hardware 6, licence 3) |
| VST3 | 0 | 3 | 120 | 123 | 192 (UAD 174, demo 16, silent 2) |
| **all** | **156** | **197** | **381** | **734** | **389** |

- The path: 156 / 188 / 390 (§11) → +7 from the anchor repairs, +2 from J37 (m/s) → 156 / 197 / 381. AMEK EQ 250 on the finer grid would make it 198 / 380 (§12.3).
- **Reproduce** (in `~/Desktop/EJMAP_TRUST_17oct`): `REPAIRED_MAPS=$PWD/repair_17oct/maps EXTRA_CERT=$PWD/rulings2_cert:$PWD/alldaws_cert:$PWD/repair_17oct/readback_cert CERTIFIED_PROFILES=$PWD/published_v3/EJMAP_PUBLISH_COMPRESSORS_v3 CERT_OVERNIGHT=$PWD/overnight_cert_rederived ./recount.sh <ejmap> <out>`. The script now prints the AU / VST3 split.

### 12.2 The format bridge (pin WZ-BRIDGE-1)
- **The rule.** A VST3 inherits its AU twin's certified / mapped level only when all of the following hold:
  - the twin is the same product at the same version (`versionKey`: "1.4.5 (R666666666)" = "1.4.5"; "5.4.3.17500" ≠ "5.4.3");
  - every role control of the AU map (the controls that meet its category's needs) is in the VST3 map;
  - each of those controls displays the same value in both formats at **3 or more** shared normalised settings, within the read-back tolerance (labels equal for a stepped control).
  - Any mismatch, too few points, a missing control or another version means no inheritance, and `bridge_refused` says which.
- **The plan.** `--trust-levels` writes `bridge_plan.json` and `bridge_names.txt`. The guarded run reads both formats at 10 / 30 / 50 / 70 / 90 %:
  `--readback-sweep --format AudioUnit|VST3 --at bridge_plan.json`.
- **On this Mac.** 39 VST3 maps sit below an AU twin at the same version. The guard blocks 37 of them.
  - The other 2 cannot bridge: Spectre's AU drive role is not identified, and Solid Bus Comp's VST3 Threshold display never moves (it shows 3.9 dB whatever is written).
  - **0 inherit.** The 33 blocked products with a plan go to Sean (12.4).
- **Also read** (the VST3s the guard allows, 16): 3 mapped (Heatwave, Quantum, TB_Sibalance_v4). The rest:
  - held: 6;
  - role not identified: 5;
  - ValhallaVintageVerb, which fails on a **judge defect found here, not fixed**: its display switches from ms to "26.75 s", and the judge compares the bare 26.75 against 26750 ms anchors;
  - Solid Bus Comp, whose VST3 Threshold does not display.

### 12.3 The anchor repair (pin WZ-REPAIR-1): `ejmap --anchor-repair --maps <dir> --plan <json> --out <dir> [--grid 0.0125] [--timeout <s>]`
- **What it does.** It rebuilds the named controls' anchors from the plugin's own display, in one guarded load per product, on a 1/80 grid.
  - The grid **leaves out** every setting the read-back then judges: 25 / 50 / 75 % and the old failures' norms, ±0.004. So the read-back is never a reading of an anchor.
  - A control the map lacks is added by index, only when the probe's parameter there carries its name.
  - It needs 4 or more readings, one unit family, and a display that moves.
  - The map store is never written; the repaired copies sit in `~/Desktop/EJMAP_TRUST_17oct/repair_17oct/maps_final`.
- **The 10 AudioUnit maps.** All 10 failing controls were rebuilt and read back at their failures' own settings. 7 are now mapped:
  - VComp (s/m) Input;
  - Q1 (m) Band 1 Frq (the old map had no anchor above 0.95 and a straight line through an exponential);
  - F6-RTA (s/m) and REQ 2 (s);
  - LinEQ Lowband (s).
- **AMEK EQ 250 HF Freq 1.** Its display has a kink at 0.875 (17.5 → 18.0 → 18.8 kHz). On the 1/80 grid it still missed, by 150 Hz (tolerance 126). One finer grid (1/160) was tried for it alone, with the same held-out settings, and all four held-out reads pass. Both runs are kept (`repair_17oct`, `repair_17oct_amek_fine`). **It stays unsure in the count** pending your ruling on the finer grid (12.5).
- **API-550A (s/m) Low Gain stays unsure.** Its parameter is continuous but its display, like the hardware, moves in steps (-12, -9, -6 … +9, +12 dB). Interpolated anchors can never match a step. It needs a stepped-display representation (a ruling).
- **The missing role controls:**
  - **J37 (m) and (s) Saturation:** added, read back → **mapped**.
  - **Solid Bus Comp Threshold (AU):** added, 78 anchors; the AU is certified already.
  - **Harris Doyle Mid Gains (4):** the plugin prints **no text** for them at any setting (a 12-step parameter). No display map can be built; it needs an audio measurement.

### 12.4 Sean's pack: `~/Desktop/SEAN_VST3_PACK_17oct.zip`
- **The plugins:** the 124 guard-blocked VST3s (PACE / iLok 81, SSL 22, Kilohearts 12, Soundtoys 9; UAD 175 left out, since they need the hardware run).
  - Step 1 reads each back as a VST3.
  - Steps 2-3 run the bridge on the 33 with a plan, AU and VST3 at the same five settings.
- **How it runs.** `--allow-licence-bound-list` lifts the guard **only** for the listed products, and stops if a window appears. It uses a scratch copy of his ledger (never `config.json`), stops at 07:00, and resumes on the next run. It only measures; the judging happens here.

## 13. Rulings 3, 17 Oct (Kathy). Built, pinned, run on this Mac (guarded); nothing sent; no server change

### 13.1 The adaptive grid (pin WZ-REPAIR-2): `ejmap --anchor-repair ... --refine <first read-back cert>`
- **The procedure** (one finer round, never repeated):
  1. Repair on the standard 1/80 grid.
  2. Held-out read-back.
  3. Only a control whose held-out read-back **failed** is re-sampled once, four times finer (1/320). The re-sampling goes around each failing setting (one grid step either side) and around each anchor-slope jump (an interval over 2× both neighbours' slopes). The held-out settings stay out.
  4. A fresh held-out read-back. The control counts only if that passes.
- **Run on all 13 repaired controls** (`~/Desktop/EJMAP_TRUST_17oct/repair2_17oct`: `pass1`, `readback1`, `pass2`, `readback2`, `maps_final`):
  - 12 passed on the standard grid and were not refined.
  - AMEK EQ 250 HF Freq 1 failed. It was refined: 10 finer anchors around 0.875. It passed the fresh read-back and **now counts**.

### 13.2 Stepped displays (pin WZ-STEPS-1)
- **Detection.** A display is stepped when it has 8+ readings, mostly (60 %+) on plateaus of 3+ equal values, and the jumps between plateaus are larger than its last digit. Rounding of a continuous display (one-digit jumps) is not steps.
- **The map.** The control becomes `"kind": "steps"`, with `"steps": [[lo, hi, value, display], ...]`: each step's read norm range.
  - Its `anchors` are the steps' middles, so a dial by anchors lands mid-step: **dial the middle of a step**.
- **The judge.** A reading is judged at the step its norm falls in. Between two read ranges, either neighbour is right.
- **The read-back.** Taken at the middles of the steps at 25 / 50 / 75 % of the list.
- **API-550A (s/m) Low Gain:** 11 steps, read back → **mapped**.

### 13.3 The ms → s unit switch (pin WZ-TRUST-17)
- **The rule.** A display in seconds against a map control kept in ms is ×1000; ms against s is ÷1000. The map control's own `unit` decides, so a map without one is judged as before.
- **ValhallaVintageVerb VST3 Decay** ("26.75 s" against anchors in ms) → **mapped**.

### 13.4 The wizard's VST3 lane (pin WZ-VST3-1): "measure once per product on AU, then bridge-read the VST3 twin"
- **Discovery** loads nothing: every *.vst3 under /Library and ~/Library/Audio/Plug-Ins/VST3, with its name, version and vendor from the bundle.
  - A WaveShell VST3 is reported, not enumerated; its plugins are measured through their AudioUnits.
- **Identity.** Each other VST3 is loaded **once**, after the same checks an AudioUnit gets (remembered licence windows, PACE on a Mac with no iLok, an APB unit not connected).
  - The probe now prints the class's name, version, JUCE uid and vendor. On this Mac, Pro-Q 3 VST3 = `ba5f7ab2` / `3.2.3.0`, the same as its existing VST3 map.
- **Twin** (an installed AU of the same class name): never measured. **Bridge read** instead:
  - the role-named controls both formats carry (at most 12), at 10 / 30 / 50 / 70 / 90 %, one load per format;
  - written as `ej_readback_sweep/1` records with `"purpose": "bridge"`, `format`, `plugin_id` and `twin_plugin_id`;
  - sent in the bundle (`readback/bridge/`, manifest `bridge_reads`). The server judges the bridge (`trust::bridgeFor`).
  - Live on this Mac (scan only, `--only "Pro-Q 3"`): 859 VST3 bundles found, 1 loaded; 12 controls × 5 settings read in both formats, **60 / 60 identical**.
- **No twin.** The VST3 is measured as its own plugin: its own map (`format: VST3`), its own rows (`plugin_id` `VST3|uid|version`) and its own Phase B.
  - Phase B children find it through `installedPlugins()`, i.e. the AU registry plus `EJ_VST3_EXTRAS`.
  - **With the variable unset (every cert run, Sean's builds) the list is the AU registry alone, as before.**
  - Record stems and identities take their format from the plugin (`fmtTag`): 48 hard-coded "AudioUnit" sites in the cert driver now follow the plugin. The tuner (pitch) path stays AU; the wizard does not measure tuners.
  - No guard-allowed VST3-only product exists on this Mac, so this lane is pinned (fake runner, a VST3 extras file) but not run live.
- **PACE.** `paceHeld` reads a .vst3 bundle directly; one it cannot find is held, not assumed clear.
- **Uploads.** Every manifest entry carries `format` (from its plugin_id); rows carry `format`.
- **Bug caught by the pin.** `vst3Extras()` read its list out of a temporary (dangling), so it returned nothing. Fixed: held by name.
- **Cost on a tester's Mac.** One load per non-shell VST3 bundle for its identity, plus 3 loads per twin. On this Mac's 859 bundles that adds tens of minutes to the scan unless a server lookup can settle a VST3 by bundle and version first. **For A / Sean:** a VST3 identity cache keyed by bundle path + CFBundleVersion would make it once per install.
- **`EJ_WIZARD_NO_VST3=1`** turns the lane off (rehearsals).

### 13.5 What Sean's server must accept (not sent; for `FOR_SEAN_server_items`)
1. **plugin_id `VST3|<uid>|<version>`** everywhere a plugin_id goes (lookup, routes, rows, profiles, outcomes, licence memory). uid = the JUCE VST3 class hash (hex), version = the class version (e.g. `3.2.3.0`). Record stems `VST3_<uid>_<version>`.
2. **`format` on every contribution entry** (profiles, confirmations, skipped, crashed, needs_category, other_categories, out_of_scope, held, reference votes) and on rows. Wizard parameter maps may carry `identity.format: "VST3"` (schema `ej_param_map/wizard-0`).
3. **`bridge_reads`** in the contribution manifest (`[{product, format, plugin_id, twin_plugin_id, file}]`) plus the `readback/bridge/*.readback.json` files (schema `ej_readback_sweep/1`, `purpose: "bridge"`). The server applies the bridge rule:
   - a VST3 inherits its AU twin's level only with 3+ matching paired settings per role control, at the same normalised version (`1.4.5 (R…)` = `1.4.5`; `5.4.3.17500` ≠ `5.4.3`).
4. **A map control of `kind: "steps"`**: `steps: [[lo, hi, value, display]]`; anchors = step middles. EchoJay dials the middle of the step holding the wanted value, and a read-back is judged at the step.
5. **Repaired controls:** `trust: "repair"` and a `repair` block (`source`, `grid_step`, `held_out`, `version`, `measuredAt`, `previous_anchors` | `added`, `refined`). Top-level `repaired_controls`.
6. **Read-back fields:** `readback.snapped`, `readback.failed_at`, `readback.superseded`. Trust rows carry `format`, `bridged_from`, `bridge_refused`.
7. **The read-back judge's unit rule:** a control's `unit` (ms / s) scales a display in the other time unit.
8. **Certification by format:** a profile certifies only its own format (`plugin.format`). The 47 published are AudioUnit.

### 13.6 Recount, Kathy's count rule, 17 Oct after rulings 3

| format | certified | mapped | unsure | testable | can't test on this Mac |
|---|---|---|---|---|---|
| AudioUnit | 156 | 197 | 258 | 611 | 197 |
| VST3 | 0 | 4 | 119 | 123 | 192 |
| **all** | **156** | **201** | **377** | **734** | **389** |

- **The path:** 156 / 197 / 381 (§12) → +4 mapped: API-550A s/m (steps), AMEK EQ 250 (adaptive grid), ValhallaVintageVerb VST3 (unit switch) → 156 / 201 / 377.
- **Reproduce** (in `~/Desktop/EJMAP_TRUST_17oct`): `REPAIRED_MAPS=$PWD/repair2_17oct/maps_final EXTRA_CERT=$PWD/rulings2_cert:$PWD/alldaws_cert:$PWD/repair2_17oct/readback1:$PWD/repair2_17oct/readback2 CERTIFIED_PROFILES=$PWD/published_v3/EJMAP_PUBLISH_COMPRESSORS_v3 CERT_OVERNIGHT=$PWD/overnight_cert_rederived ./recount.sh <ejmap> <out>`.
