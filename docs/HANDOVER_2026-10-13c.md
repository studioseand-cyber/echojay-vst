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

## 14 Oct addendum 6: the one-load read-back check (wizard 6bdbcd5d; probe 7f9a5971, carried to cert as da531bf0, pushed)
- **What it does:** every control a read-back check needs is read in one probe load, each restored after its reads. One load per control is used only on a refusal.
- **Before / after (the same verdicts):** EMO-D5 (m) 184.5 -> 19.1 s, Q4 (m) 115.1 -> 25.1 s, API-550A (m) 59.8 -> 13.1 s, CLA-76 (m) 13.6 -> 9.0 s.
- **This Mac's first scan:** about 3.7 h -> about 1.4 h at 2 jobs (read-back checks 2.8 h -> about 31 min). The first run in total is about 29.3 h at 2 jobs, 21.2 h at 4.
- **Pin WZ-QC-1:** mutants Q1-Q4 red. Q1 needed the pin to gain a refusal INSIDE the multi block.
- **DMGs at 6bdbcd5d:** Test sha 0f34bd8e28c2ab3c, Beta sha feead47077ce94ee; both signed and verify VALID; the probe re-signed.
- **Noticed, not changed:** EMO-D5 and Q4 fail the read-back on display rounding ('14996' vs '15000', '1925' vs '1924'), so they go on to be measured in full. A numeric tolerance in `wizard::readback` would let such plugins pass as ready from another version. That changes what counts as a match, so it is Kathy's call.

## 14 Oct addendum 7: the near-match tolerance, and a PACE-check regression fixed (wizard 07a6a8e2)
- **The tolerance** (WIZARD_SPEC 3k, pin WZ-NEAR-1, mutants M1-M5 red):
  - **The rule:** numbers in the same unit within max(0.5%, one step of the coarser last shown digit); text and units exactly. Wizard only.
  - **The record:** a plugin passing this way is recorded `matched_with_tolerance`, with each near-match in the upload / results file for the audit.
  - **Here:** EMO-D5 (m) and Q4 (m) are now ready (3 and 36 near-matches); API-550A (m) matched exactly.
  - **All 123 hostable read-backs:** 66 exact, 37 within tolerance, 20 measured. 956 near-matches, 4 over 0.5% (HF Freq 42/41 on H-EQ / H-EQ-Light: the one-step rule on a whole-number display). 4 one-load reads were refused and fell back per control.
- **REGRESSION FIXED (since 8332b253; the DMGs from then on had it):**
  - **The cause:** the PACE check reads a bundle's whole binary, and WaveShell2-AU 12.0 (589 plugins) was re-read for each. A plan of this Mac took over 20 min, and on testers' Macs the scan would have sat at "Looking for plugins".
  - **Now:** each bundle is read once, and only where it can matter. A scan of the 471 settled plugins takes 13 s; a plan takes 50 s.
- **This Mac, whole first run:** about 26.6 h at 2 jobs / 19.9 h at 4.
  - The tolerance's share: 37 fewer plugins to measure, about 0.4 h (mostly fast EQs).
  - The earlier 29.3 h counted every read-back plugin as a measurement.
- **DMGs at 07a6a8e2:** Test sha b9e85d140efc68cc, Beta sha fc7323065a446e47; both signed and verify VALID.

## 14 Oct addendum 8: the 1% ceiling (wizard 224a9016), and where the measuring time goes (report only)
- **The ceiling** (WIZARD_SPEC 3k, pin WZ-NEAR-1; the no-ceiling mutant is red):
  - **The rule:** two numbers never match past 1% of the larger, whatever the last-digit step says. 10/11 and 42/41 fail; EMO-D5 and Q4 still pass.
  - **The 123 read-backs:** 66 exact / 33 tolerance / 24 measure (was 66/37/20). The four that moved are H-EQ and H-EQ-Light (m/s).
  - **The plan:** 522 -> 526 to measure; the estimate moves about 1 minute (25.28 -> 25.30 h at 2 jobs; the whole first run is still about 26.6 h).
  - **Checked in the shipped engine:** H-EQ (m) queued on 'HF Freq' 42 vs 41; EMO-D5 (m) ready (3 within tolerance).
- **Where the measuring time goes on this Mac** (report; nothing changed):
  - **Two real runs here:**
    - CLA-2A (m) v12: 496 s, 58 processes, 8.6 s each. Sean's v15: 78 s, 57 processes. The wizard's figure is 340 s.
    - H-Reverb (5->5) v12: 1220 s, 281 processes, 4.3 s each. Sean: 335 s, 371 processes. The wizard's figure is 41 s.
  - **Why:** all 250 Waves plugins to measure here sit in WaveShell2-AU 12.0, which is x86_64-only, so every process is bridged. A bridged load takes 3.6-4.1 s (native: 0.08-0.15 s).
  - **The model:** Sean's seconds and process count per plugin (cert 5; the category median where he has none), plus 3.5 s per bridged Waves process. It is calibrated on the two runs and is good to roughly +-40% per plugin.
  - **Result:** about 35 h of work and **about 19.7 h at 2 jobs** (12.1 h at 4), against the wizard's 25.3 h.
  - **By step:**
    - loads 65% (22.8 h; 20,233 of 37,702 processes are bridged Waves);
    - sound-test renders 21%;
    - the compressor path's hold-doubled repeat 6%;
    - tone passes 6%;
    - demo checks 2%;
    - waits and retries 0 (slept_ms 0 and attempt 1 on all 20,664 of Sean's compressor processes; 0 retried here).
  - **The starting figures are off in both directions:**
    - Reverb is 41 s against 20-28 min for each Waves H-Reverb, so the bar sat at "under a minute left" for 20 min.
    - Strips are 1200 s against a 29 s median on Sean's Mac.
  - **Three savings that keep accuracy (none made):**
    - **(1) One process per plugin pass, with a fresh instance per position.** This keeps the 29 Sep fresh-state rule. About 19.7 -> 10 h. It needs an A/B identity test first (townhouse).
    - **(2) 4 jobs on this Mac.** It has 8 performance cores but 3.1 GB free, under the 4 GB rule. About 19.7 -> 12.1 h. It needs a per-job memory measurement first.
    - **(3) Native UAD out of the serial lane.** About 2 h. It needs proof of which UAD plugins use no DSP. Licence-bound, so it cannot be tested here.
    - **Not counted as a saving:** skipping the hold-doubled repeat (about 2 h). It is the trust gate (ruled 2 Oct).
- **DMGs at 224a9016:** Test sha f097e7c24d2aad68, Beta sha cef12a16209edfaa. Both signed and verify VALID; the probe is unchanged and already Developer ID signed.

## 14 Oct addendum 9: faster measuring (wizard 7648fef1 + 774fe72e; cert 26b3cbaf, pushed)
- **The cheap test (14 Oct, this Mac):** a bridged (x86_64-only) AU costs 3.7-3.9 s for EVERY new instance in an arm64 process, because macOS starts its bridge host again each time. With one idle instance held open, each new one costs 0.035-0.09 s. Run as x86_64 in-process: the library load is 3.5 s, an instance 0.025-0.07 s. Native townhouse: 0.37 s then 0.27 s.
- **The warm probe** (WIZARD_SPEC 3l, pin WZ-SERVE-1 / cert PROBE-SERVE-1):
  - The probe's `--serve` runs a pass in one process: an idle keeper instance, plus a NEW instance per call.
  - `runChild` sends calls to it under `EJ_PROBE_SERVE=1`.
  - It is the wizard's default (`--no-probe-serve` turns it off). EJ Map's `--phaseb-all` only uses it with `--probe-serve`.
  - Mutants red: the answer keeping the newline, the load output kept, never served, the load death kept warm, no restart on any path. Two single-path restart mutants survive, because the restart has three independent paths.
- **A/B (this Mac, one job):**
  - H-Reverb 1218 -> 141 s, H-EQ 408 -> 86 s, CLA-2A 497 -> 226 s, townhouse 256 -> 252 s; 2443 -> 770 s in all.
  - Classified equality, 0 unexplained:
    - H-Reverb and H-EQ are identical.
    - CLA-2A's records and level readings are identical; a per-window field differs <= 0.0006 dB, exactly as between two one-shot runs.
    - townhouse varies run to run by the same amount both ways (RMS 0.0032 dB old/old and warm/warm; 0.0032-0.0039 old/warm).
    - The rest are clocks, timestamps and paths.
  - Shipped engine check: H-EQ (m) with the Test app's bundled engine and probe, `--serve` seen, 91 s, records identical to the old way.
- **Jobs:** a job peaked at 29-89 MB old and 39-146 MB warm (top, every ~9 s). Reserved at 300 MB: 4 jobs need 1.5 GB + 4 x 300 MB = 2.7 GB free (was 4 GB). This Mac had 3.1-4.1 GB free today, so it gets 4. Pin WZ-JOBS-1.
- **The bar:** `beta_db/expected_work.json` (scripts/expected_work.py, from Sean's cert 5, read only) weighs each plugin by Sean's seconds, or the kind's median, and the Tracker learns this Mac's ratio. Pin WZ-BAR-1.
- **No time on screen:** confirmed. The engine's "under a minute left" is in its log and progress.json line only; WZ-NOTIME-1 now pins that the app never reads it.
- **This Mac's measuring:** about 27.5 h at 2 jobs before -> **about 16.3 h at 2 jobs / 12.1 h at 4** (calibrated on the A/B). Compressors are now the largest part (about 14 h of work: rendering under the bridge). The UAD serial lane is unchanged (7.3 h).
- **DMGs at 774fe72e:** Test sha 99dd8ced44e9ab95, Beta sha 16921f7b8de7528f. Both signed and verify VALID; the probe is re-signed after its rebuild.

## 15 Oct addendum 10: one Start, the measuring screen, the label (wizard 92f34301)
- **What it fixes:** the 14 Oct overnight Test run finished Checking at 23:07 (2 h 15 min) and then sat on "Ready to measure - 504 plugins - Start" until 07:15.
- **Now (WIZARD_SPEC 3m):**
  - One Start carries the whole run (WZ-START-1).
  - The measuring bar shows a % with a shimmer, and the "measured so far" line is gone (WZ-SCREEN-1).
  - The name under the bar is the newest plugin and is never cut (WZ-LABEL-1).
  - An unrun UAD row reaches the bar.
  - 12 mutants red.
- **Found on 15 Oct (not changed; proposals in the report):**
  - Phase B leaves every UAD row unrun without a UAD-2 device (158 in this run's queue), while the scan counted only 18 as needs_hardware. The scan should use the same device check.
  - APB (McDSP) plugins measure silence without APB hardware: 4 rows took 3-8 min each in the serial lane. Proposal: a device preflight like UAD's, giving needs_hardware at the scan.
  - Checking does not use the warm probe; it would save about 1 h of the 2 h 15 min.
- **Timing CSV:** ~/Desktop/WIZARD_TIMING_15oct.csv, refreshed every 10 min (scratchpad timing_csv.py, read-only on the wizard's files).
- **DMGs at 92f34301:** Test sha 8cedf599bc2c2970, Beta sha 11f1b4c1ba4928e3. Both signed and verify VALID. Not installed: today's Test run is the 774fe72e build.

## 15 Oct addendum 11: the hardware check at the scan (wizard 70801a29; DMGs Test 673d9d7b, Beta 77b74b9e)
- UAD without a UAD-2 device and APB without its unit are needs_hardware at the scan, on every route that measures or reads sound (WIZARD_SPEC 3n, pin WZ-HW-1, 4 mutants red).
- The plan's count now holds only what will load.
- This Mac (a plan, nothing loaded): 220 needs_hardware (212 UAD, 8 APB) and 341 plugins to measure (was ~504).
- APB detection has never been seen with a unit. A network-only unit may need `--assume-apb-device`.

## 15 Oct addendum 12: surround variants out of scope (wizard 6e2841b6; DMGs Test 2754e61b, Beta de414add)
- Surround-only variants are out of scope at the scan: not loaded, not measured, not counted (WIZARD_SPEC 3o, pin WZ-SURROUND-1, 4 mutants red). They are read from the name.
- This Mac: 86 variants (29 had been queued). New plan: 312 to measure. Today's L360 (5->5) / (6->6) compressor passes ran about 50 min each.

## 15 Oct addendum 13: before tonight's fresh run (wizard d8568f87; DMGs Test f71a2cff, Beta 3b1e4a9f)
- UAD is measured by sound, not by device (EJ_UAD_NATIVE). UAD loads here show UA's authorisation window: closed, needs_licence in ~4 s, remembered.
- A sound check in the demo check files silent or never-processing plugins at once.
- The reward totals exclude needs_hardware and surround.
- The pause says "Paused at N%".
- The warm probe now runs in Checking too (A/B identical, 199 -> 103 s on 9 plugins).
- WIZARD_SPEC 3p; 10 mutants red.
- Not carried to cert: the shared-code changes (the UAD gate's env switch, ScopedServe) change nothing in EJ Map's own runs.

## 15 Oct addendum 14: sharing the Mac (wizard 81ec5a2d; DMGs Test 5a7d5c3d, Beta 5d1c14e3)
- **What changed** (WIZARD_SPEC 3q, pins WZ-GUEST-1 / DAW-1 / ACTIVE-1 / RETRY-1, 7 mutants red):
  - The engine, its children and the probes run as background guests (nice 10, utility disk I/O; the engine was seen at nice 10).
  - A DAW pauses the scan or the measuring by itself and it carries on by itself.
  - 2 jobs while the user is active.
  - A timeout is retried alone before it is filed.
- **Not carried to cert:** the probe and the Phase B pool changes are switched by EJ_WIZARD_BACKGROUND / retryTimeoutsAlone / jobsCap, all unset in EJ Map's own runs.
- **Today's (15 Oct) Test run finished at 15:24** (3 h 49 min of measuring after the 07:15 Start, with Kathy's 10:02-11:35 pause) and went to the "no code" screen: "351 of 509 plugins have a result so far".
  - The 158 missing are the UAD rows the 774fe72e build left unrun without a UAD-2 device. The d8568f87 build decides them by sound (needs_licence or measured), and 92f34301 reports an unrun row to the bar.
  - The watcher's "warm-probe restarts" on the strips (CLA MixHub ~50) are each strip section's own child starting its own warm probe, not deaths: every row finished ok.

The standing rules are unchanged (HANDOVER_2026-10-13 §6).
