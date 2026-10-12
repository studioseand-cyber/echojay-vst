# Custom defaults: the census and the plan (12 Oct)

Only the **detection** is built (`EjmapCustomDefault.h`, pins CD-1..3, 3 mutants red). The rest is a proposal for Kathy's decision.

## a. How big it is (`census.txt`, `coverage.txt`)

**Inputs**
- **Sean's side:** the parameter block every probe call prints at load, taken from his raw traces only when nothing was written first. That gives 1006 products.
- **This Mac:** the same block from EJ Map's categorise read (`resp30`, `norms=current`, nothing written).

**Comparable:** same plugin id AND the same version. That is 327 products; 512 others have a different version here. 28 could not be read here (a window at the load, no automatable control). **299 were compared.**

| result | count | which |
|---|---|---|
| identical opening values | 290 | |
| values differ, but not in sound | 3 | Pro-Q 3 (an analyzer display toggle), Purple Audio MC 77 (a VU readout), MChorusMB ("Sine" / "Sine": the stored number differs within one step) |
| **values differ for real** | **6** | **all MeldaProduction:** MTurboCompMB (30 controls), MMorph (20), MUnison (16), MFlangerMB (7), MTurboComp (5; its "Device" selector opens as Meldei 1176 on Sean's Mac and MSL E… here), MTuner (2) |
| same values, names formatted differently | 80 | Melda's parameter-naming setting (e.g. "Compression (A)" vs "A - Compression"); not a difference |

**So:** at the same version, 6 of 299 (2%), all from one vendor whose plugins load a default preset from a per-user library.

**Coverage limits**
- Only plugins installed on both Macs at one version were compared.
- PACE units were read with their windows closed, never clicked.
- Licence-bound vendors were not loaded here.

**What this Mac shows about Melda** (read-only): `~/Library/Application Support/MeldaProduction/DefaultSettings` is empty, so no user-saved default here. The per-plugin preset libraries (`MMorphpresets.xml`, owned by root) date from a 2021 install, while the plugins are 14.16. One possible cause is a difference in the installed preset library rather than a user's saved default. These files do not say which Mac shows the factory state.

## b. The proposal (both cert and the wizard)

1. **The reference state.** For each plugin and version, the database keeps a reference instantiate block: index, norm, name, text. It comes from the cert sweep's `instantiate_seen` or the probe's load block. It is stored with its provenance (which Mac, source, date) and how many contributors opened with the same block. Profiles carry `instantiate_reference_fp` (a hash of the block), so the server knows which state a profile assumes.
2. **Before measuring**, the probe's opening block is compared with the reference (`customdefault::compare`, built). Then:
   - **Same** → measure.
   - **Differs, and every differing control can be written** → write each one back to the reference norm as a **neutral write on the record** (listed as `custom_default_restored`), read the block again, and measure only if it now matches. The record says "opened with a custom default (N controls), restored to the reference".
   - **Differs, and it can't be restored** → file **`custom_default`** and do not measure. This covers a control that isn't automatable, a write that doesn't land, a state that isn't in the parameters at all (a preset-only selector, hidden state), or a re-read that still differs. The record lists the differing controls. The wizard files it quietly (it is not an error) and sends the list for review.
3. **Which state is factory when two Macs disagree**, strongest first:
   - **The plugin's own factory preset.** Load AU factory preset 0 (`kAudioUnitProperty_FactoryPresets`; in JUCE the AU's programs) in the probe, and print its block. The Mac whose opening block equals it shows factory. This needs a small probe mode (`--factory-preset N`), not built. Some plugins define no factory preset, or one that isn't their init state; then use the next test.
   - **A fresh install on a clean user account:** a real second macOS user, not a HOME override (that breaks Plugin Alliance licences). That account has no vendor library and no saved defaults, so the opening block there is the binary's own init.
   - **The vendor's default file:** a user-saved default sitting in the vendor's place (Melda `DefaultSettings` / per-plugin default presets, FabFilter `Default.ffp`, Waves preset settings, …) marks that Mac's state as custom.
   - **The majority across contributors** (once the wizard has users): the block most Macs open with at a version is the reference, and the minority is flagged. This is a fallback, not proof, since one vendor's library update could move everyone.
4. **For Melda specifically** (all 6 real cases): until the factory-preset read exists, treat a Melda plugin whose opening block disagrees with the reference as `custom_default`. Or Kathy decides to restore by writes.

## Decisions for Kathy
1. Restore by writes and measure, or always refuse (`custom_default`) when a plugin opens differently?
2. The source of truth for factory: build the probe's factory-preset read (recommended), or rely on a clean-account read?
3. Treat name-format differences as no difference (as built)?

## Kathy's decisions (12 Oct, later) and the results

**2. The factory preset was built, validated and rejected as the reference.**
- `ejmap --factory-read --only …` runs the probe's `--factory-preset [default|N|open]`: the opening block, the unit's factory presets, then preset N's block, with no audio.
- **Validated** on the 289 plugins that open identically on both Macs (`factory_validation.txt`):

| result | count |
|---|---|
| publish no factory preset | 91 |
| first preset equals the opening state | **93 of 198 = 47%** (32% of all 289) |
| first preset is a sound preset that differs | 105 |

- Even init-named presets are not reliable: Lindell SBC's own "Default" differs from its opening state in 7 of 22 controls.
- That is far below the 95% bar, so it is **not used as the reference**.

**What is used instead:** the opening block that **independent Macs agree on**. It is trusted at 2 or more sources (`kTrustedAgreeing`). Here that means Sean's traces plus this Mac, which give 290 trusted references now. More come as contributors open the same plugin and version. One source, or sources that disagree, is not trusted.

**3. The action is built (Phase B prepare, cert and the wizard; pins CD-4/5).** It applies wherever `<out>/references/<stem>.reference.json` exists:
- opens as the reference → measured;
- differs from a **trusted** reference → every differing control is written back at the reference's norm, read back through the probe (the writes are passed as `EJ_PROBE_PRESET` on that one call), then measured with the same writes as the child's `--preset` under every probe process. The row records `custom_default_restored` (control, opened, written);
- the read-back does not match, or the reference is not trusted → **`custom_default`**, not measured (counted with the skips, not the failures);
- no reference file → unchanged, so Sean's next run behaves as today.

**Closed later on 12 Oct (Kathy's decisions + handover):**
- **The vote** (e6a3cbf4, CD-6): every check casts one. Identical to a single-source reference → measured, and the vote makes it trusted (`makes_trusted`). It differs → `custom_default` (held), and the vote carries this Mac's opening block for a third source. Rows record `reference_vote`. On the wizard, the votes travel in the manifest's `reference_votes[]` and the stub counts them (89e59f5c, WZ-CD-2).
- **Strips** (e6a3cbf4, CD-7): the restore writes come first, and a section's own write replaces only a control it sets. `preset_merge` on the section says which won, per control.
- **The compressor-certification batch** (b24085d5, CD-8): `runCertSweep` runs the same check (`certSweepReference`, sharing `referenceCheck` with Phase B). `custom_default` → `<cert>/custom_default/<stem>.json`, not measured (exit 4). Restore → `EJ_PROBE_PRESET` for that product's probes, cleared after, plus `<stem>.restored.json`. A restore already in force (Phase B's children) is not re-checked. No reference file → unchanged.
- Not re-run live after the b24085d5 refactor: Phase B's prepare now calls `referenceCheck`, which CD-8 drives end to end through a fake probe.
