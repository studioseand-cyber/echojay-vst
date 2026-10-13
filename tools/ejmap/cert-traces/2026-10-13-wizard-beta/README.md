# Wizard: ready for beta - the rehearsals, 13 Oct (feat/ejmap-wizard)

All runs used the SIGNED apps (`assemble.sh --sign`; the app, its engine and its probe all have TeamIdentifier 8BT5F9B887).
- **Fresh state:** each run had a fresh work folder and a fresh store (`<work>/store`), with the mock DB (4 profiles, the categories, the 290 references) as the stub server's db.
- **This Mac's store was never touched:** `~/Library/ejmap` kept every file's path and modification time (1123 maps before and after).
- **The 13-plugin set:** elysia mpressor, MFreeformEqualizer, SPL De-Esser Dual-Band, bx_limiter, bx_rockrack V3 Player, Unfiltered Audio G8, spiff, OTT, bx_crispytuner, ADA STD-1, bx_rooMS, Lindell 50 Channel, and Lindell SBC (in the mock DB).

| run | build | what | result |
|---|---|---|---|
| **R1** | beta | the 13, end to end, `--auto` | Welcome → consent (unticked, then ticked) → scan → "You have 10 plugins we don't have yet" → measuring 29 min 45 s → **EchoJay-Wizard-results-2026-10-13.zip** (36 KB): 8 profiles / drafts. MFreeformEqualizer filed **demo** (dropouts, 24 dB) and skipped; the bundle carries no path and no user name. 1816 s in all |
| **R2** | normal | the 13, account A, end to end | Sign in → consent → scan → eligible (10, minimum 5) → measuring → upload: the server counted **accepted_new 8** from the zip, so reward_eligible → code EJ-FREE-9465A7A5. The account and the device hash were recorded as taken part |
| **R3** | normal | 3 plugins, a fresh server | count 3 < 5 → "Thanks - your plugins are already mapped", with Contribute anyway / Done → Done: nothing runs, nothing recorded |
| **R4a** | normal | account A again, the same Mac | /lookup at consent: already_contributed → "You've already taken part - thank you"; **nothing loads** (no scan) |
| **R4b** | normal | account B, the same Mac | refused the same way (the Mac's device hash) |
| **R4c** | normal | account C, a "second Mac" (`EJ_WIZARD_FAKE_DEVICE`) | allowed: scan, eligible (8), measuring; closed 5 s in (SIGINT, exit 130) and reopened: it **carries on** in measuring (a resumed run, allowed) and records nothing until an accepted upload |
| **R5** | engine | SIGINT 2.5 s into a fresh scan | exit 130, "Stopped - it carries on next time: 3 of 12 plugin(s) checked"; 0 markers, no never-retry list. The next run reused the 3 maps and finished: 1 ready, 10 to measure, 1 demo |
| **R6** | engine | the disk floor (`EJ_WIZARD_FAKE_FREE_FILE`) | 2.0 GB free: exit 6, "needs 3.0 GB free and this Mac has 2.0 GB". Started at 5 GB, dropped to 0.5 GB 15 s into measuring: the next 10 s check stopped it; mpressor in flight was discarded (no row, it runs again); "Stopped - your Mac is running out of disk space (0.5 GB free...)"; no markers, no bundle |
| **R7** | normal | Ctrl-C to the APP during the scan | "Ctrl-C: the same stop as the close button" → the engine stopped (130) → the app quit; 0 markers, no never-retry list, only `consented` saved |
| **R8** | normal | the window at 2.0 GB free | "Not enough disk space": the refusal, before anything starts |

**Screens:** `screens/` (01-06 beta; 10-15 normal). The logs are in `logs/` (paths as `<scratch>` / `~`), with `r1_results_manifest.json` and `r2_receipt.json`.

**Found and fixed during the rehearsal:**
- **Both builds returned the literal name "EchoJay Wizard".** JUCE's one-instance guard is keyed on it, so the second app to start quit silently. Each build now uses its own name (pin WZ-NAME).
- **"Already taken part" was only asked after the scan.** The scan loads every plugin to map it. The once-only answer depends only on the account and the Mac, so the app now asks right after consent (`afterConsent`, in pin WZ-ELIG-1).

**Small and not done:** the disk-refusal screen has no button; the user closes the window.
