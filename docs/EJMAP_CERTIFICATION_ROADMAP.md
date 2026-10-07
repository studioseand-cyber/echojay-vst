# EJ Map certification roadmap: beyond compressors and tuners

Drafted 4 Oct 2026 for Kathy and Sean. This is the order we work through **after** compressors and tuners are finished. Each category gets a short spec agreed with Sean before any code, the same way compressors did.

Counts are from Sean's Mac census (4 Oct): installed AudioUnits by category, not yet certified.

---

## 0. Finish first: compressors and tuners

### Compressors

- [ ] Follow-up build items 1–17. These cover the linked-pair, leader/follower, master-over-trims and main-over-aux rules, the tone-check state, licence relabelling, roles fixes, the out-of-scope states, the one-time review pick, the range rebuild and the H-Comp false licence flag.
- [ ] Dry run over Sean's zip. Check the projected counts.
- [ ] Sean builds and signs the follow-up commit, then runs the tone-check session (about 10–15 s per product).
- [ ] Publish once, from the follow-up. Hold Lindell 254E and VBC FG-Grey until they re-pass.
- [ ] Re-run once activated: the 9 Melda plugins, Low Control and Pro-C 3, after Sean's three Logic checks.
- [ ] Investigate and re-run the 12 flat or passthrough products: RComp, SSLComp, C1 comp, VComp, NEOLD U2A, U17, dbx-160 (m) and SSL Fusion HF.
- [ ] Fix and re-run API-2500 (m/s), where the repeat pass never ran, and Purple MC 77, where the curve dips.
- [ ] Make the one-time review picks for the genuine choices: Shadow Hills ×2, Maag, MaxxVolume ×2, VBC Rack, Auto-Tune Vocal Compressor and dbx-160 (s).
- [ ] Re-run the UAD units with the UAD-2 Satellite connected (Sean, 6 Oct: a Satellite, not an Apollo; it runs at the host's rate): 41 compressors and tuners.
- [ ] CL 1B's 0.7 dB live gap: wait for A's live quiet-window reading.

### Tuners

- [ ] Auto-Tune Artist and EFX: lengthen the speed test, because they don't settle within the 1 s flip.
- [ ] Auto-Tune Access: test only the three Retune Speed detents (Slow, Medium, Fast).
- [ ] Agree a tuner spec with Sean, so the records reach the server.
- [ ] Re-run the 4 UAD Auto-Tune units with the UAD-2 Satellite connected.

---

## 1. Two kinds of certification

A control needs certifying when its label doesn't tell you what it does to the audio. That happens in two ways:

| Kind | What it means | Measurement | Cost |
| --- | --- | --- | --- |
| **Level-dependent** | The effect depends on how loud the incoming signal is (thresholds, drive) | The full level ladder, as for compressors: each setting × many input levels, hold test, tone check | Higher |
| **Label accuracy** | The control does the same thing at any level, but the label may be wrong | One sweep of the control at one level, plus a second level to confirm it really is level-independent | Lower |

Level-dependent: compressors, limiters, gates, de-essers, transient shapers, saturation and amp drive, multibands.

Label accuracy: output, trim and make-up gain; EQ gain, frequency and Q; mix knobs; reverb decay; delay time.

---

## 2. The work, in order

### 2.1 Gain and output controls on every plugin (label accuracy)

**Why:** every level match on every track depends on them. The server's make-up step (spec §6.0 rung 4) writes them on every compressor. It also fixes the range gaps B counted: 42 controls with a non-numeric end and 76 with an end sample missing.

**Measure:** a 997 Hz sine at −20 dBFS. Sweep each gain-role control (output, make-up, trim, and input when it isn't the threshold) at 21 evenly spaced norms, and record output minus input against the displayed value. Repeat at −40 dBFS to confirm it's level-independent. Input stages that saturate will show up here.

**Export:** `gain_curve: [{norm, display, measured_db}]` per control, a `display_matches` flag (within 0.1 dB), and full ranges with no gaps.

**Reuses:** the probe's level reading, and the range re-sample from the follow-up build.

**Ask Sean:** where it lives (map data or a profile field), and whether the server switches from display to measured dB.

**Size:** small. Seconds per product.

### 2.2 EQs: 175 (label accuracy)

**Why:** on nearly every track in every genre. Digital EQs mostly show honest numbers. Analog-style EQs often don't: gain labels aren't real dB, boost and cut interact (Pultec-style), frequencies are stepped, and Q changes with gain.

**Measure:** per band, the magnitude response (log sine sweep or a multi-tone) on a grid of settings: gain at about 7 points × frequency at about 7 points, plus each Q or shape position. Derive the actual centre frequency, the actual gain at the centre, and the bandwidth. Check two input levels for EQs that saturate.

**Export:** per band, a mapping from display to actual centre Hz, gain dB and bandwidth. Stepped frequencies list every detent.

**Acceptance:** ask for "+2 dB at 3 kHz", then measure within 0.5 dB and a stated frequency tolerance.

**New pieces:** a sweep or multi-tone signal in the probe, frequency-response analysis, and a roles pass that groups each band's controls.

**Size:** large. There are many products and many bands.

### 2.3 Compressor timing, then multibands (level-dependent)

**Why:** today's profiles say how much compression happens at a steady level. On drums and buses, attack and release decide how much of each hit gets through. The spec's `time` field is currently omitted.

**Timing:**
- **Measure:** tone bursts. Step the input 10 dB above the setting's own threshold, record GR over time, and take attack as the time to 63% of the final GR. Step back down for release. Do this per attack and release position, and flag program-dependent units (opto, auto release).
- **Export:** fill `time` per position as measured, not as labelled.

**Multibands:** about 18 in Sean's zip, plus any multibands among the uncategorised.
- **Measure:** a signal with energy in every band, either one tone per band at its centre at the default crossovers, or noise with a stated spectrum. Read GR per band and one overall figure.
- **Amount:** either a global control (depth, range, offset), or every band's threshold moved by one common dB offset. That needs a spec field.
- **Before code:** the read-only multiband survey (item 17 of the current prompt), then a spec proposal to Sean.

**Size:** timing is medium. Multibands are large, with server changes on Sean's side.

### 2.4 Limiters: 46 (level-dependent)

**Why:** on almost every master bus, and on many sub-buses.

**Measure:**
- **Ceiling accuracy:** output peak against the ceiling label, both sample peak and true (inter-sample) peak, with oversampling on and off.
- **Threshold or input gain to GR:** the compressor ladder, reused.
- **Release:** the burst test, as in 2.3.

**Export:** a limiter profile: ceiling accuracy, a GR curve and release.

**Size:** medium. Mostly reuses the compressor machinery.

### 2.5 Saturation (118) and amp sims (75) (level-dependent)

**Why:** guitars, bass, drums and the mix bus. Drive changes loudness, so "more warmth" without level compensation is really "louder", and the before-and-after comparison misleads.

**Measure:**
- **Each drive position × input level:** the output level change, and the harmonic content (2nd against 3rd, overall THD).
- **The onset:** the input level where distortion starts.
- **Amp sims with cabinets:** the frequency response as well.

**Export:** `level_change_db[drive][level]`, a harmonic summary and the onset level.

**Size:** medium to large.

### 2.6 Channel strips: 106 (both kinds)

**Why:** console-style plugins used on everything. They contain compressor, EQ, gate and saturation sections.

**Method:** split each strip into its sections by role, and certify each with its category's method from above. Section engage switches are handled like Rule 1 and the engage search.

**Size:** large. Do this after 2.2–2.5.

### 2.7 Reverb (100) and delay (38) (label accuracy)

**Why:** on most mixes. "A touch of reverb" depends on the mix law, and "a short room" depends on the decay actually being short.

**Measure:**
- **Mix law:** dry and wet level at each mix position (equal-power, linear, or something else).
- **Decay:** RT60 against its label, from an impulse or burst.
- **Timing:** pre-delay and delay time accuracy.
- **Tempo sync:** at a host tempo the probe supplies. That needs transport and tempo support in the probe.
- **Feedback:** its effect on delay decay.

**Size:** medium.

### 2.8 Transient shapers (18) and gates (8) (level-dependent)

**Why:** small counts, but central to drums.

**Measure:**
- **Transient shapers:** attack and sustain amount against the transient boost or cut in dB, on a drum-like burst.
- **Gates:** opening and closing level against the threshold label, hysteresis, range, and attack, hold and release from bursts.

**Size:** medium. It reuses the ladder and the burst test.

### 2.9 De-essers: 22 (level-dependent)

**Why:** mostly vocals, sometimes cymbals and hi-hats.

**Measure:** the compressor engine with a tone or band-limited noise in the sibilance range (4–10 kHz), giving threshold against GR. Also the frequency control's display against its actual centre, and split-band against wideband mode.

**Size:** medium. Close to compressors.

---

## 3. Across every category

- **605 uncategorised plugins:** invisible to everything. A categorisation pass on Sean's side is the biggest single gain in coverage.
- **New probe signals, built once and shared:** multi-tone and noise (2.2, 2.3, 2.5, 2.9), tone bursts (2.3, 2.4, 2.8), log sweeps and impulses (2.2, 2.7), and host tempo and transport (2.7).
- **Stereo and mid-side:** the per-channel reading and the linked-pair rules carry over. Mid-side modes need noting per product.
- **The same template for every category:**
  1. Agree a spec with Sean.
  2. Probe signal.
  3. Derivation.
  4. Export schema.
  5. Acceptance check.
  6. Tests and deliberately broken versions.
  7. Rehearsal on Kathy's Mac.
  8. Batch on Sean's Mac.
  9. Publish once.
- **Standing rules:** the machine never guesses (a human pick is recorded once, never repeated); never Send from EJ Map; zip only the cert folder; profiles key to the plugin version they were measured on (§11).

---

## 4. Suggested order and why

| # | Category | Kind | Products | Why this position |
| --- | --- | --- | --- | --- |
| 1 | Gain and output | Label accuracy | Every plugin | Cheap, universal, and fixes level matching and the range gaps |
| 2 | Compressor timing | Level-dependent | Compressors | Builds on what exists; the biggest accuracy gap for drums and buses |
| 3 | EQ | Label accuracy | 175 | On nearly every track |
| 4 | Limiters | Level-dependent | 46 | Every master; mostly reuse |
| 5 | Multibands | Level-dependent | About 18 | Mastering and buses; needs a spec change |
| 6 | Saturation and amp sims | Level-dependent | 193 | Guitars, bass, drums, mix bus |
| 7 | Channel strips | Both | 106 | After its parts exist |
| 8 | Reverb and delay | Label accuracy | 138 | Mix law and decay accuracy |
| 9 | Transient shapers and gates | Level-dependent | 26 | Drums |
| 10 | De-essers | Level-dependent | 22 | Close to compressors |
| — | Uncategorised | — | 605 | Sean's side; raise now |
