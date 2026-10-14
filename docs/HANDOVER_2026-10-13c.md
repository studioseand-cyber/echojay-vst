# Handover 13 Oct (c) - what changed since docs/HANDOVER_2026-10-13b.md

**HEADs:** `feat/ejmap-wizard` (this file's commit; pushed). `feat/ejmap-cert` is unchanged at 0b9ef77e. Nothing in this round touched the cert's measuring; the one shared-driver change (`SweepOptions::limiterPassKnown`) is empty unless the wizard sets it.

Sean's build is still b7aa304c, with tonight's line unchanged (HANDOVER_2026-10-13 §1).

## Built this round (Kathy's "before beta goes out", three passes)
1. **The beta database snapshot** (26badd11): `tools/ejmap/wizard_app/beta_db` is a read-only snapshot of Sean's cert 5, built by `scripts/beta_db_snapshot.py` and shipped in the beta bundle.
   - 678 plugin ids are already mapped: 256 profiles and 422 draft ids.
   - It also carries 1218 settled categories, 17 multibands and the 290 references (10.2 MiB).
   - The limiter compressor passes sit in `beta_db/limitercomp/` (714ac57a), so each limiter pass is looked up separately (WZ-LIM-1).
2. **Separate work folders** (26badd11): the beta uses `EchoJay Wizard Beta`; the release keeps `EchoJay Wizard`. Kathy's earlier stopped run is still in the old shared folder.
3. **`ejmap --wizard --plan`**: the database step only, nothing loaded; it reports routes and the per-category estimate.
4. **Jobs by Mac** (§3e): 2 by default; 4 with 8+ performance cores and 4 GB free.
5. **The reward rule** (§6.5, server decides):
   - qualify `>= 25` at scan time;
   - the code only for a FINISHED run, complete on the merged run, with accepted `>= 25`;
   - one code per account and per Mac.
6. **Pause / Send what's done** (§3b) on the Measuring screen and when the window is closed; "N measured so far"; a Paused screen.
7. **Partial uploads** (§3f, §6.6): a checkpoint on a pause, a close and every ~60 min. Each manifest carries the run id, queue and outcomes; the stub merges per run + plugin, idempotently. The release keeps an offline outbox. The beta rewrites its one Desktop file.
8. Two defects from the earlier "Finish now" rehearsals were fixed and pinned: "measured so far" counted results that weren't sendable (WZ-FIN-2), and a stop between two passes was ignored for a whole pass (WZ-FIN-3).

## Pins
WZ-SNAP-1, WZ-WORK-1, WZ-PLAN-1, WZ-LIM-1, WZ-FIN-1..3, WZ-TIME-1, WZ-REWARD-1..8 (and 6b), WZ-OUTBOX-1, WZ-BETAFILE-1, WZ-JOBS-1, WZ-CKPT-1, WZ-CHOICE-1. Every rule has a mutant that went red (the logs are in the commit messages). The gate is green.

## This Mac (1376 AUs; 1285 enter the scan)
| | of the 1285 |
|---|---|
| skip (nothing loads) | 381 = 222 already mapped + 159 settled utility / modulation / meter |
| measure | 624 = 479 queued + 145 rule-B read-back first |
| sound-test (signature reads) | 272 |
| held (multiband) | 8 |

**Time** for the 625 to measure (649 measurements; the per-category starting figures; a read-back check counted as a measure):
- about 27.4 h at 2 jobs, 19.7 h at 4;
- this Mac had 3.6-3.9 GB free during the session, so it would get **2 jobs**.

**Scan stage** before that: about 0.9 h at 2 jobs (904 maps at 4 s, 272 signature reads at Sean's 4.8 s mean). ~~plus a 60 s demo check per plugin~~ - WRONG, corrected 14 Oct below.

## Rehearsals (13 Oct night; hostable Waves v12 de-essers + C1 gate; work, server stub and results in scratch)
- **A (release, server default 25):** the 4 plugins counted 4 < 25 -> "Thanks - your plugins are already mapped / You have 4 plugins we don't have yet - the free month needs 25 or more. You can still contribute them." with "Contribute anyway (no reward)".
  - Its body text was changed from "nothing to do right now", which contradicted the button.
  - The release rehearsals seed the stub server's db with the snapshot's categories.json (the server's catalogue); an empty stub db sends these 4 to review.
- **B (release, qualify_minimum 3 as a server setting):**
  - Launch 1 offline: the close question was snapped, Pause at 1 measured, the checkpoint was kept in the outbox, then the Paused screen.
  - Launch 2 online: the outbox was sent at launch, the same run carried on with checkpoints at 1 -> 3 -> 4 of 4, then the finished upload -> code EJ-FREE-8E7AE524.
  - Receipts: accepted 1, 1 (+0, a duplicate), 3, 4, 4; reward only on `finished`.
  - Found and fixed: the Paused screen's Quit was disabled, and it said "has been sent" while offline. A stale checkpoint.json was re-sent at launch while the new engine's first checkpoint was skipped (an engine's seq restarts) -> checkpoints are now keyed on their time stamp across launches.
- **C (release, Send what's done at 1):** Send what's done at 1 measured -> the `sent_early` upload: 1 of 4 final, accepted 1, no code: "The free month needs the full run, so there's no code this time." (The no-code screen no longer repeats its title in the body.)
- **D (beta, pause at 1, then finish):** Pause at 1 -> the Desktop file written (1 draft), Paused ("Your results so far are in the file on your Desktop: even if you stop early, email it."). The relaunch carried on and finished -> the SAME file rewritten (4 drafts, still one file); the finish screen says "even if you stop early, email the file on your Desktop".

## The DMG
`~/Desktop/EchoJay-Wizard-Beta-2026-10-13/EchoJay Wizard Beta.dmg` - built at 48116637, sha256 0329c6c41c8c265f..., signed 8BT5F9B887 (not notarised), hdiutil verify VALID. The bundle carries beta_db with 52 limitercomp files, the icon is identical to WizardBeta.icns, and the engine strings carry 48116637. README_BETA.md (repo + Desktop) is updated.

Evidence: `tools/ejmap/cert-traces/2026-10-13-reward-partial/` (screenshots + receipts).

## Open (for Kathy / Sean)
- ~~The demo check's time~~ - settled 14 Oct (below).
- **SERVER (Sean):** /contributions partial uploads + the run merge (WIZARD_SPEC 6.6), the earn rule, qualify_minimum 25.
- **The close question** has been snapped (`--auto-ask-close`), never clicked by a person.
- **The hourly checkpoint** was rehearsed at 0.5 min, not at 60.
- **Notarisation** (Apple's agreement).

## 14 Oct addendum: the demo check (wizard f20b46e1)
- **Measured:** the 13 Oct "60 s per plugin, ~4 h in the scan" was wrong. The 60 s tone renders offline.
  - The wizard's own probe call, timed in the engine: 4.0-7.3 s per plugin, mean 4.5, on 8 Waves v12 plugins (the same at 1 and 2 jobs). The load is most of it. Melda demos took 0.31 s.
  - For ~625 plugins that is about 30 min at 2 jobs, not 4+ h.
- **Moved** into each plugin's measuring (WIZARD_SPEC 3, "Where and what it costs"): the scan no longer runs it, the estimate carries it, Pause / Send what's done stop it, and "measured so far" ignores it. Pin WZ-DEMO-2; mutants D1-D5 red.
- **Rehearsed** (beta; DeEsser + C1 gate + the Melda demos MTurboComp and MLimiterX): the scan took 5 s. A pause 2 s into measuring came after MTurboComp was filed demo, and the checkpoint was packed. The resume filed MLimiterX demo just before its compressor pass, measured the other two, and rewrote the Desktop file.
- **This Mac, corrected totals:**

| | 2 jobs (what it gets now: ~3.7 GB free) | 4 jobs (8 P-cores + 4 GB free) |
|---|---|---|
| scan (maps + 272 signature reads; no demo check) | ~0.9 h | ~0.6 h |
| measuring (649 measurements + 625 demo checks; the scan-done estimate) | 27.9 h | 20.1 h |
| **total** | **~28.8 h** | **~20.7 h** |

- **DMG:** `~/Desktop/EchoJay-Wizard-Beta-2026-10-13/EchoJay Wizard Beta.dmg` rebuilt at f20b46e1, sha256 95093958a28c3ce8..., signed 8BT5F9B887 (not notarised), hdiutil verify VALID; icon, beta_db (52 limitercomp) and hash checked.

## 14 Oct addendum 2: no times on screen (wizard 842dc876)
- **Kathy's rule:** no user-facing time in either build. That covers scan-done, the Measuring bar (now "n of N" measurements), the Measuring text, the close question, the Paused screen and README_BETA.
  - Scan-done ("Ready to measure"): "We found N plugins we haven't mapped yet. Measuring them runs by itself in the background - it's best left running overnight, and you can pause any time and carry on later." The release adds "Finish them all to get your free month."
  - The estimate stays internal: `to_measure_eta_s` (logged at scan done), `progress.json`, `--plan`. The hours above are for us, never for a tester.
- **Pin WZ-NOTIME-1** checks 41 strings, the README bullets and the app source. Mutants T1-T5 are red. Screenshots: `tools/ejmap/cert-traces/2026-10-14-no-times/`.
- **DMG** rebuilt at 842dc876: sha256 cf1b5162c69e870d..., signed 8BT5F9B887, hdiutil verify VALID; the icon and the hash are checked, and the Desktop README has no "every hour".

## 14 Oct addendum 3: EchoJay Wizard Test, and licence windows (wizard 8332b253)
- **The Test build** (654a7402): target `ejmap-wizard-test`.
  - It runs the release flow against the stub server inside the app, seeded with the beta snapshot. Any sign-in works.
  - The "TEST VERSION - nothing leaves this Mac" banner is on every screen. It has its own folder (`EchoJay Wizard Test`) and an orange TEST icon.
  - The server's settings are in `server_stub/config.json` (qualify 25 by default; edit and reopen). **Reset test** keeps that config. It shows wherever nothing runs: the app reopens on the reward screen after a finish.
  - Rehearsed: sign in, Pause, reopen, finish -> code, Reset. Pin WZ-TEST-1.
  - DMG + README: `~/Desktop/EchoJay-Wizard-Test-2026-10-14/` (sha 9458cf0f29a73bf7, signed, verify VALID).
- **Licence windows** (8332b253, WIZARD_SPEC 3g, pin WZ-LIC-1, mutants L1-L6 red):
  - **Remembered:** a window at any load (`engine/licence_windows.json`) means that plugin is never loaded again.
  - **PACE:** skipped unloaded ONLY on a Mac with no iLok at all (no USB key AND no PACE licence software). A missing USB key alone never skips, so computer-licensed plugins load (Kathy's check, 14 Oct). Melda's optional Eden wrapper is exempt.
  - The scan-screen warning. The probe was already background-only; measured that the frontmost app doesn't change while it loads.
- **This Mac now:**
  - The scan loads 905 plugins (+272 signature reads). 39 of them are PACE-wrapped (Melda excluded), and each could show one window, once ever.
  - The PACE rule skips 0 here, because PACE software is installed.
  - The 13 Oct run's 9 window products (all PACE-wrapped): 8 are now already mapped and never loaded; 1 loads (SSL Fusion HF Compressor).
  - Before (empty db, 13 Oct): 1285 loads + 1210 reads, 108 PACE, on every scan.
- **The beta DMG** was rebuilt at 8332b253: sha 51cb06442e9aba1b, verify VALID; README_BETA carries the licence line.
- **Housekeeping, 14 Oct:** this Mac hit its process limit. Pioneer's FwUpdateManagerd had 2273 zombie children; it was stopped with Kathy's OK.

## 14 Oct addendum 4: the small run, and WaveShells (wizard 46255daa)
- **Small run (Test build only):** `test_only_plugins` and `test_qualify_minimum` in `server_stub/config.json`. README_TEST has the example (DeEsser, RDeEsser, Sibilance, C1 gate, TransX Wide + minimum 3).
  - Rehearsed with exactly that config: a finish and the code in 7.5 min, 5/5 accepted.
  - The beta and release ignore both keys: the server override is switched on only by the Test build, and the beta binary doesn't contain the code.
  - Pin WZ-TEST-2.
- **WaveShells** (WIZARD_SPEC 3h, pin WZ-WAVES-1):
  - **The shells:** only v12 shells here; WaveShell2-AU 12.0 holds 589 of 616 Waves plugins.
  - **CLA-76 (m)** loads in 3.7 s every time. There is no hang, no first-load penalty, no shell mismatch and no licence window on this Mac.
  - **The 49 s scan** was the read-back check loading it per control; it is now 10 s via the missing-control short cut.
  - **The guard:** a 30 s WaveShell limit; one hang or two crashes skips the whole shell's plugins for the scan with "Waves plugins skipped - check Waves Central". Rehearsed with a forced limit.
  - **Kathy's message arrived as item "5." alone; items 1-4 never reached this session.**
- **DMGs at 46255daa:**
  - Test: `~/Desktop/EchoJay-Wizard-Test-2026-10-14/` (sha 6f575a5e1d4fb542).
  - Beta: sha 691c6442bbd62315.
  - Both are signed and verify VALID.

## 14 Oct addendum 5: Kathy's stuck Test run (wizard 4fad4d2a)
- **What happened** (her logs, read-only):
  - **CLA-76 (m)** sat 45 s in the 12-load read-back check of Sean's 15.0.70 profile against this Mac's 12.0.0 (no 'Mix').
  - **The empty bar:** the read-back checks ran inside Scanning (stage 0, where the bar's value is 0), and that loop never checked for a stop.
  - **Orphan engines:** both of her closes left the engine running. One engine was still loading at 12:00 (Q6 (m)); it was stopped with her OK and needed SIGKILL, because the old build ignores SIGINT and SIGTERM there.
  - **A wrong never-retry entry:** the next launch put API-550A (m) on the never-retry list as a "crash".
  - **The check times:** read-back checks averaged 70 s each here (max 180 s, EMO-D5).
- **Fixed** (WIZARD_SPEC 3i, pin WZ-SCAN-2, mutants N1-N5 red):
  - The database step loads nothing.
  - Read-back checks run in Checking: stoppable, and cached in `quick_checks.json`.
  - One n-of-N count: N excludes the out-of-scope-by-type plugins that load nothing (1285 here). The bar starts at the settled share (about 30% here).
  - Orphaned engines stop themselves (measured about 2 s), and the app stops its engine when it goes.
  - Screenshots: `cert-traces/2026-10-14-scan-count/`.
- **Her folder:** RESET TEST is recommended. It holds the wrong never-retry entry, and it was scanned by the old build. Eject the old mounted Test image first, install the new DMG to Applications and open it from there.
- **DMGs at 4fad4d2a:** Test sha 3de41460527111f5, Beta sha 58228c00530ba023; both signed and verify VALID.
- **Still slow, not changed:** a read-back check loads the plugin once per written control (EMO-D5: about 48 loads). Batching the reads into one probe call would make each about 2 loads. It needs a probe change, which is for Kathy to approve.

The standing rules are unchanged (HANDOVER_2026-10-13 §6).
