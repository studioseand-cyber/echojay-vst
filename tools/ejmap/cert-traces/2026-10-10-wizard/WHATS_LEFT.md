# The wizard: what is built (feat/ejmap-wizard, 10 Oct) and what is left before it is a real app

## 13 Oct (c): before the beta goes out - built
- Jobs by Mac (2; 4 with 8+ performance cores and 4 GB free), the estimate at the chosen jobs.
- Limiters: the limiter pass and its compressor pass looked up separately (beta_db/limitercomp/).
- The reward rule (server decides): qualify 25 at scan; the code only for a FINISHED run, complete on the merged run, accepted >= 25; one per account and Mac.
- Pause / Send what's done on the Measuring screen and on close; "N measured so far"; a Paused screen.
- Partial uploads: a checkpoint on a pause, a close and every ~60 min; run id + queue + outcomes in each manifest; the stub merges per run + plugin; offline outbox.
- Beta: the one Desktop file rewritten at every checkpoint; the finish screen and README say "even if you stop early, email the file on your Desktop".

## Left (13 Oct c)
- SERVER (Sean): /contributions partial uploads + the run merge (WIZARD_SPEC 6.6), the earn rule, qualify_minimum 25, the outbox-safe idempotency.
- The close question (pause / send) is snapped in a rehearsal (`--auto-ask-close`), but a person has not clicked it.
- The hourly checkpoint was rehearsed with `--checkpoint-minutes 1`; a real 60-minute interval has not been watched.
- Notarisation (Apple's agreement).


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

- ALL CATEGORIES (12 Oct): limiter (+ limitercomp), de-esser, saturation / amp sims, gate, transient, multiband, tuners, reverb, delay
  and strips through EJ Map's own Phase B categories on the wizard's per-category queues (`SweepOptions::wizardQueues`), in Kathy's
  order (compressors first, strips last), one Phase B pass per group. Estimates: Sean's measured medians per category. Bundle: each
  category's cert draft. Pins WZ-CAT-1..4. Rehearsal: cert-traces/2026-10-12-wizard-all-categories/.

- THE SERVER BOUNDARY (12 Oct, Sean's review; `EjmapWizardServer.h`, pins WZ-SRV-1..5): base URL https://www.echojay.ai; uploads
  over 4.5 MB through the signed blob (upload-url -> PUT -> complete), under it direct (both stubbed and tested); the 12 server
  categories in Sean's spellings via ONE table at the boundary (de-esser, channel_strip, transient_shaper, pitch, amp_sim from a
  cabinet read); multiband held (listed with its map, not measured), modulation / meter / utility / instrument never sent;
  a manifest with any other category is refused before packing; a drive-less saturation draft is not sent.
- SERVER-SIDE, AWAITING SEAN (stubs only): the account -> mapper-token bridge; the review / holding area; promo rules + issuing + email;
  the reference-vote count (one per machine, trust at 2, keep disagreeing blocks, return `reference` in /lookup).

- KNOWN MULTIBANDS + CUSTOM DEFAULTS (12 Oct): the 17 known multibands held at once (WZ-MB-1); a plugin opening differently from its
  reference is restored by writes (read back) or filed custom_default (cert's Phase B check, carried; WZ-CD-1, CD-4/5). The reference
  must come from the server (/lookup `reference`); the factory preset is NOT a reference (47% match). Rehearsal:
  cert-traces/2026-10-12-wizard-custom-defaults/. Melda's paid plugins are unlicensed here (demo dropouts).
  CLOSED 12 Oct (later): the compressor-certification batch runs the same check (cert b24085d5, CD-8); a strip's restore writes
  survive its sections' writes (e6a3cbf4, CD-7). THE VOTE (Kathy, confirmed): every check casts one; the manifest carries
  `reference_votes[]` (an agreeing vote on a single-source reference = the second Mac: measured, and the stub's receipt names the
  reference as now agreed); a differing plugin is HELD (`held[]`, held_reason custom_default) with its opening block in its vote for a
  third source. The stub tallies votes per upload (server_stub/reference_votes.json + references/<file>); WZ-CD-2, 4 mutants red.
  SERVER-SIDE: de-duplicate votes per machine / account (the stub does not).
  13 Oct (Kathy): the MOCK DB carries the 290 agreed references (wizard_app/mock_db/references; the 12 Oct census: Sean's Mac + Kathy's,
  266 full blocks, 24 Melda with the controls both Macs show); the app copies them into its stub db at start, never over a file a vote
  moved (WZ-REF-SEED). Mock DB only: no cert folder gets them; Melda's 6 that differ stay held (Kathy).

## Done for the beta (13 Oct)
- **Stop:** clean in every stage. "Stopped - it carries on next time" after a close or Ctrl-C while scanning, checking or measuring. Nothing in flight is filed as crashed, and no marker is left.
- **A stranger's Mac:**
  - demo versions are found by their dropouts and skipped;
  - licence and iLok windows are closed, never clicked;
  - a 3 GB floor to start and a clean stop below 1 GB.
- **Consent screen** with a required "I agree".
- **The beta build:** the results as one file, "Show in Finder", "Email this file to ...", no sign-in, no promo.
- **Eligibility** decided by the server: shown at the scan, granted on ACCEPTED results.
- **Once only** per account AND per Mac.
- **Packaging:** the app, engine and probe signed with Developer ID 8BT5F9B887, plus a .dmg. **Not notarised**; testers do the quarantine step in README_BETA.md.
- **Rehearsed end to end:** cert-traces/2026-10-13-wizard-beta/.

## What is left: three lists

### Ours (EJ Map / the wizard)
1. **Notarise and staple** the app and the .dmg the day Apple's agreement is accepted (`docs/NOTARISATION_SETUP.md`); then the quarantine step goes. Auto-update (Sparkle or the website) is not designed.
2. **Replace the stubs** once Sean's endpoints exist: sign-in (token into the Keychain), /lookup (with eligibility, already_contributed and references), /contributions (both paths, retry / resume of an interrupted upload), /promo. Send the device hash with /lookup and /contributions.
3. **UI:**
   - a Pause button besides closing the window;
   - "overnight" scheduling;
   - VoiceOver labels on the bar, and dark mode;
   - localisation;
   - the ETA's starting figures from the server for a first run;
   - two plugins in flight when the wizard goes down are BOTH skipped today.
4. **Demo detection beyond Melda:** measured on Melda's demos only. Other vendors may mute differently (noise, beeps, a gap after more than 60 s). Widen it from the first testers' results.
5. **Categorising on a stranger's Mac:** the signature reads run at the instantiate state.
   - A compressor with a long release can read as a tail.
   - The same plugin can read differently on two Macs.
   - A categoriser change is its own task (EJ Map's equality would move).
6. **Strips and maps:** the wizard's maps are parameter lists only (no roles); a strip's sections may not resolve. The dial side must accept a role-less map, or the server supplies roles.
7. **Run in the user's real home:** a relocated or sandboxed app makes Plugin Alliance units silent (rehearsed). Keep the user's real home visible to the probe.
8. **Windows:** everything under the window is Mac-only today:
   - AUs, the licence-window watch, the keep-awake, the process-group stop, the crash-report search, the bundle;
   - VST3 identity is instance-only.
9. **What only another Mac can test** (README_BETA.md):
   - licence and iLok windows;
   - no store and no licence file;
   - other vendors' demos;
   - the quarantine step on a real download;
   - real low disk;
   - Intel.

### Sean's (the server)
1. **Endpoints** (WIZARD_SPEC.md 6): `POST /sign-in` (and the account to mapper-token bridge), `POST /lookup`, `POST /contributions` (direct up to 4.5 MB, else signed blob), `POST /promo`.
2. **Eligibility** (6.5): the NEW CONTRIBUTABLE count with `reward_minimum` (configurable, default 5), and `eligible` / `count` / `minimum` in /lookup.
3. **The grant on ACCEPTED results:** recompute from the bundle, with no duplicates and nothing already known; below the minimum, no code and a plain message.
4. **Once only** (`once_only`, configurable): `already_contributed` per account AND per device hash, recorded on an ACCEPTED upload only.
5. **Abuse limits:**
   - one reward per account and per Mac;
   - idempotent uploads on bundle_sha256;
   - never the client's count.
6. **Reference votes:**
   - one per machine or account;
   - trust at 2;
   - keep the disagreeing blocks;
   - return `reference` in /lookup.
7. **The review / holding area,** then agreement, then publish; the per-category publish side (each draft kind and its acceptance as the send gate).
8. **Promo issuing and the email.**
9. **The Apple Developer Program agreement** on his account (the notarisation 403).

### Kathy's (decisions and text)
1. **Approve the consent wording** (WIZARD_SPEC.md 2b), and decide whether it names the device hash (proposed: yes for the release, no for the beta).
2. **Confirm the beta address** (`EJ_WIZARD_BETA_EMAIL`, today `beta@echojay.ai`).
3. **The reward minimum** (default 5) and once-only (default on) - both server configuration.
4. **Legal and privacy:**
   - a lawyer's read of vendor licence terms on publishing measurements;
   - the user's terms: what is sent, that measurements may be published, the reward's conditions;
   - a privacy notice for the account link, the device hash and the crash reports;
   - retention and deletion on request.
5. **Who gets the beta,** and how their results files are collected.
6. **Two plugins in flight when the wizard goes down:** re-run the second alone once?
