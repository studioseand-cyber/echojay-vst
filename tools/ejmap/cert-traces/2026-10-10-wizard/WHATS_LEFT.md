# The wizard: what is built (feat/ejmap-wizard, 10 Oct) and what is left before it is a real app

## Built (CLI, pinned, rehearsed here: 5 compressors + 3 EQs; and on an EMPTY map store)
- `ejmap --wizard --db <profiles folder> --out <work> [--only NAME]... [--jobs N] [--measure] [--bundle]`
- NO MAP STORE NEEDED (10 Oct late): plugins the store lacks are mapped by the wizard itself - the probe's parameter list (--jobs, the
  serial lane for UAD / PACE), the join key echojay::fingerprintForDescription (desc, param count) - the same key Sean's maps carry, so a
  stranger's Lindell SBC / Maag EQ4 matched Sean's profiles exactly - and the category (the server's catalogue first, else compressor /
  EQ words only). Written to the wizard's own store (`ej_param_map/wizard-0`: identity, parameter list, fp, category; no roles).
- Scan installed AUs -> look up by plugin_id + version + map_fp (from the store, the wizard's maps included) -> A exact / B quick live
  check (every control the profile writes read back at its norms; no audio) / C queue.
- TWO CATEGORIES: compressors and EQs (10 Oct late). EQ: the EQ profile's bands (gain / freq / Q maps by control) are the B check's
  writes; the queue runs through Phase B's `wizard_eq` (--cert-eq); an EQ is ready to send when a band is measured and every acceptance
  write that ran passed; the bundle carries the ej_eq_profile/1 draft.
- Measure the queue through Phase B's `wizard` category: the full compressor path + tone check, --jobs, the serial lane, resumable,
  atomic rows, every cert outcome (needs_licence / window / needs_device / silent_output / probe_crashed / unhostable / empty_param_list).
- Bundle: profiles + tone-check sidecars (tone check g 2 PASS only) + manifest (new profiles, confirmations of other versions'
  profiles, skipped with reasons); every file checked for home / user / host names and credential-like fields before packing.
- Upload: STUBBED (prints what it would send; nothing leaves the Mac). Plain-words summary at every stage.
- PROGRESS (11 Oct, `EjmapWizardProgress.h`, pins WZ-PROG-1..5b): `<out>/progress.json` (`ej_wizard_progress/0`) rewritten atomically on
  every event and every 2 s while measuring - percent, stage (Scanning / Checking / Measuring / Packing / Done, or Stopped), the plugin(s)
  being worked on, the stage's done / to do, problems, elapsed, an ETA from MEASURED per-category seconds (rows on disk + this run;
  --jobs aware; starting figures from this Mac until then, labelled as such) and the plain line. The terminal shows one live line,
  e.g. `[########------------]  41%  Measuring 2 of 4 - bx_opto (2 at a time) - about 4 min left`, and a final summary; every detail
  line goes to `<out>/wizard.log` (`--verbose`: the old output). Fixed stage bands + a hold-at-highest guard: the bar never goes back;
  a running plugin earns its time run (to 90%); a resumed run starts at the done share (41% after one of four compressors landed).
  Ctrl-C while measuring: no bundle, no "Done" - `Stopped at 41%: 1 of 4 measured, 3 left (the 2 being measured start again) ... Run
  the same command to carry on`, and progress.json says Stopped at that percent. A stop reports the DONE share only (Phase B discards
  the rows in flight), which is exactly where the resume opens - the one place the bar goes back, and the line says why. The
  rehearsals found three defects, all fixed and pinned: a stopped run packed a bundle with 3 "measuring" and said Done (WZ-PROG-5);
  its progress.json said 100% / Done (5b); it said "Stopped at 66%" and the resume opened at 41% (5c).
  Rehearsal: cert-traces/2026-10-11-wizard-progress/.

- THE WINDOW APP (11 Oct, `tools/ejmap/wizard_app/`, spec `docs/WIZARD_SPEC.md`): "EchoJay Wizard.app", five screens + crash
  recovery, the bar driven by progress.json, the crash marker (engine/inflight, written before every load) -> the never-retry list
  (skip.json) + a redacted crash report in the upload, keep-awake (IOKit PreventUserIdleSystemSleep while the engine runs), close =
  pause (SIGINT to the engine's process group) and reopen = carry on. Sign-in / lookup / upload / promo are LOCAL STUBS. Pins WZ-APP-1..3.
  Rehearsal: cert-traces/2026-10-11-wizard-app/.

- NO PLUGIN LEFT BEHIND (12 Oct, `EjmapWizardRoute.h`, spec section 3 + 6.2 / 6.3): the database first (rule A and the new
  pending_review are never loaded), then the server's category, the name rules, out of scope (not loaded), and only then EJ Map's
  signature reads (`categoriseOne`, shared with --categorise-propose, cached per plugin_id). Unsettled -> `needs_category` (map + reads +
  leaning); other categories -> `other_categories` (map + category); out of scope -> listed. `loads.log` records every load. Pins
  WZ-ROUTE-1..5. Rehearsal: cert-traces/2026-10-12-wizard-no-plugin-left/.

## Left before it is a real app
1. **UI**: BUILT as the v1 window app (above). Still open: a Pause button besides closing the window; "overnight" scheduling;
   accessibility (VoiceOver labels on the bar); dark mode; a first-launch explanation of what is measured and why; localisation;
   the ETA's starting figures should come from the server for a stranger's first run; two plugins in flight when the wizard goes
   down are BOTH skipped today (spec section 3: re-run the second alone once? Kathy to decide).
2. **Sean's endpoints** (exact requests / responses in WIZARD_SPEC.md section 6): sign-in (`POST /sign-in`, token -> Keychain),
   the lookup (`POST /lookup`: the server decides A / B / C / out_of_scope and the category), the upload (`POST /contributions`,
   multipart manifest + zip, idempotent on the bundle's sha256, a genuine check in seconds), the promo (`POST /promo`, one code per
   account, emailed). Server side beyond the endpoints: the holding area, agreement with another version or a second user, then
   publish; the reward ledger. Client side once they exist: replace the four stubs, retry / resume of an interrupted upload.
3. **The real database lookup**: "what's missing" on the server (plan section 8) instead of a local folder; the server, not the client,
   decides A / B / C, and records B confirmations and live-check misses.
4. **Categories beyond compressors and EQs**: de-esser / saturation / reverb / delay / transient / gate / limiter / tuner / multiband /
   strips need their profiles published and their lookups defined (the EQ pattern: the profile's written controls for the B check, the
   category's acceptance as the send gate). And a category for a plugin neither the catalogue nor its name places: today "not measured
   by the wizard yet" - the measurement categoriser (--categorise-propose) could decide it on the user's Mac.
5. **Licence handling for strangers**: the user's own licences.csv does not exist - the wizard relies on window / PACE detection and the
   serial lane; a "skip anything that asks for a licence" default and a plain explanation screen.
6. **Signing + notarisation**: today the app and the engine are signed AD HOC by assemble.sh (only the probe carries the Developer
   ID). For download: Developer ID on the app, the engine and the probe (hardened runtime, the probe's entitlements kept), then
   notarise and staple the app (and a DMG). The engine loads no plugin itself (the probe does), so it needs no library-validation
   exception; the probe already has its entitlements. A first-run Accessibility prompt (the licence-window watch) needs explaining.
   Auto-update (Sparkle or the website) is not designed.
7. **Windows**: the window is JUCE and would build there, but everything under it is Mac-only today: AUs (a VST3 probe is needed,
   and VST3 identity is instance-only - see the P15 note), the licence-window watch, the IOKit keep-awake (SetThreadExecutionState),
   the process-group pause (a Job object + CTRL_BREAK), the crash-report search (WER LocalDumps), the app bundle. Or Mac-only first
   (plan section 9).
8. **Legal / privacy**: what the bundle carries is listed in the spec (profiles, tone checks, EQ profiles, a manifest with the OS
   version and the probe's cdhash, REDACTED crash reports - home path, user, host and /Users/ paths replaced; a report that still trips
   the check is withheld). Needs: a lawyer's read of vendor licence terms on publishing measurements of their plugins; the terms the
   user accepts (what is sent, that measurements may be published, the reward's conditions); a privacy notice for the account link and
   the crash reports; data retention / deletion on request; whether crash reports (third-party plugin stacks) may be kept.
8b. **Categorising on a stranger's Mac**: the signature reads run at the plugin's INSTANTIATE state, so (a) a compressor with a
   release reads as "a tail" first (Lindell 254E -> "reverb or delay: review", 12 Oct) - the tail rule runs before dynamics; (b) the
   same plugin can read differently on two Macs (MMorph: eq on Sean's, dynamics here); (c) settled "dynamics" goes to the compressor
   certification, which then decides (MAutoVolume: refused, Melda demo-mode dropouts). Sean labels needs_category from the reads; a
   categoriser change is its own task (EJ Map's equality would move).
9. **Field hardening**: the empty-store path is built and rehearsed; still open - sleep / lid-close handling in "overnight", disk-space
   checks before measuring, and the wizard's maps are parameter lists only (the GUI's role mapping is not run: the dial side must accept
   a map without roles, or the server supplies them).
10. **Run as the user, in the user's real home**: plugins find their activation under the user's home folder. Rehearsing with HOME pointed
   at an empty folder made every Plugin Alliance unit pass digital silence (4 of 4 filed silent_output, not sent - the filing worked). A
   sandboxed or relocated app would do the same: the app must keep the user's real home and Application Support visible to the probe.
