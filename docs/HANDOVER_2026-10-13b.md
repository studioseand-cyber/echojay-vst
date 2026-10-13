# Handover 13 Oct (b) - what changed since docs/HANDOVER_2026-10-13.md

**HEADs** (both pushed):
- `feat/ejmap-cert` 0b9ef77e, plus this file.
- `feat/ejmap-wizard` e6667cdb, plus this file.

Sean's build is still b7aa304c, with tonight's line unchanged (HANDOVER_2026-10-13 §1).

## Changed since
- **Consent edits** (wizard 52f877d4): Kathy's wording, pinned word for word (WZ-FLOW-1); WIZARD_SPEC.md 2b holds both texts.
  - The release says the results "become part of EchoJay's shared plugin map, used to help EchoJay control plugins for everyone who uses it", plus the hashed-ID sentence.
  - The beta says it "saves the results for you to send to EchoJay", plus "If you send it, the results become part of EchoJay's shared plugin map."
  - The release text fills the consent box's 9 lines exactly.
- **PB-GUARD-2 margin** (cert 0b9ef77e, wizard 66a4ee45): its re-declared guard went from 6 s to 30 s. The gate had failed once under a load average of 33 with no FAIL line printed; the cause was not proven. The read-once mutant is still red.
- **The scan bar** (wizard 024c3485):
  - **The bug:** the scan screen drew the engine's overall percent ÷ 15, and Checking is only 10-15% overall, so Kathy's run showed about 67% at "15 of 1285".
  - **Now:** the bar is the Checking stage's own n of N (`wizapp::scanBarValue`), and a resumed scan starts at its true share (`Tracker::alreadyDone`).
  - **Pins:** WZ-SCANBAR / -2.
- **The app icon** (wizard f2dce27a): `wizard_app/icon/make_icon.swift`.
  - **The mark:** the EchoJay mark ("o" with its arc), REDRAWN AS VECTORS fitted to the logo bitmap (no source larger than ~200 px exists on this Mac), white on the ink rounded square.
  - **Beta:** a blue BETA ribbon.
  - **Wiring:** the iconutil .icns (10 sizes), ICON_BIG on both targets, installed by assemble.sh. Pin WZ-ICON.
- **Kathy's earlier beta run on this Mac:** stopped cleanly at 42 of 1285 (19:27:50), in `~/Library/Application Support/EchoJay Wizard`. Do not run it in full here: it would load the licence-bound vendors.

**The beta DMG:** `~/Desktop/EchoJay-Wizard-Beta-2026-10-13/EchoJay Wizard Beta.dmg` (built at f2dce27a, signed 8BT5F9B887, not notarised), with the tester-only README_BETA.md. Evidence: `tools/ejmap/cert-traces/2026-10-13-wizard-beta/` (wizard branch).

**Known limit:** this process has no Screen Recording permission, so window screenshots are refused. Use the app's own `--snapshots`, or NSWorkspace icon renders.

## PENDING for the next session (Kathy, 13 Oct)
1. **The beta database snapshot.** Ship the beta with a read-only snapshot of what Sean's cert 5 knows (`~/Downloads/cert 5`, read-only):
   - **Contents:** every exported profile and measured Phase B draft as "already mapped" by plugin id + version + map fp; every settled category; the multiband list; the 290 references.
   - **Excluded:** no tokens, no personal paths.
   - **Report:** how many of the 1285 plugins on this Mac it would skip / measure / sound-test, and the snapshot's size.
   - **Pin** the lookup against it.
2. **Separate work folders** for the beta and release builds: `Application Support/EchoJay Wizard Beta` vs `EchoJay Wizard`. Today both use the second; WizardApp.cpp `parseArgs` builds `c.work`. Kathy's stopped run lives in the shared folder.
3. **Then:** rebuild the signed beta DMG in `~/Desktop/EchoJay-Wizard-Beta-2026-10-13/`. Check the result: the icon, a scan screenshot via `--snapshots`, and a strings check of the hash.

The standing rules are unchanged (HANDOVER_2026-10-13 §6).
