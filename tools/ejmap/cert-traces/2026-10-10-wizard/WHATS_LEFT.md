# The wizard: what is built (feat/ejmap-wizard, 10 Oct) and what is left before it is a real app

## Built (CLI, pinned, rehearsed on 5 hostable compressors here)
- `ejmap --wizard --db <profiles folder> --out <work> [--only NAME]... [--jobs N] [--measure] [--bundle]`
- Scan installed AUs -> look up by plugin_id + version + map_fp (map_fp from the local map store, read only) -> A exact / B quick live
  check (every control the profile writes read back at its norms; no audio) / C queue.
- Measure the queue through Phase B's `wizard` category: the full compressor path + tone check, --jobs, the serial lane, resumable,
  atomic rows, every cert outcome (needs_licence / window / needs_device / silent_output / probe_crashed / unhostable / empty_param_list).
- Bundle: profiles + tone-check sidecars (tone check g 2 PASS only) + manifest (new profiles, confirmations of other versions'
  profiles, skipped with reasons); every file checked for home / user / host names and credential-like fields before packing.
- Upload: STUBBED (prints what it would send; nothing leaves the Mac). Plain-words summary at every stage.

## Left before it is a real app
1. **UI**: a window (scan list, "map now / overnight", progress, review-and-Send). Today a CLI; the JUCE app shell exists (MainComponent)
   but no wizard screen. Estimated hours / ETA from real times per category.
2. **Sign-in + upload**: per-user accounts and tokens (plan section 5) - server side (Sean): auth, the contribution intake (holding
   area, validation, agreement with another version or a second user, then publish), the reward ledger. The client's upload call,
   retry / resume of an interrupted upload, and the Send confirmation.
3. **The real database lookup**: "what's missing" on the server (plan section 8) instead of a local folder; the server, not the client,
   decides A / B / C, and records B confirmations and live-check misses.
4. **Categories beyond compressors**: the wizard measures compressors only (the published profile type). EQ / de-esser / saturation /
   reverb / delay / transient / gate / limiter / tuner / multiband / strips need their profiles published and their lookups defined.
5. **Licence handling for strangers**: the user's own licences.csv does not exist - the wizard relies on window / PACE detection and the
   serial lane; a "skip anything that asks for a licence" default and a plain explanation screen.
6. **Notarisation** (not just Developer ID signing) of the app and the probe; a first-run Accessibility prompt explained.
7. **Windows**: AUs are Mac-only - a VST3 probe + window watch for Windows, or a Mac-only first launch (plan section 9).
8. **Privacy review**: the manifest's `platform` (OS version) and the profiles' `measured.tool` (probe cdhash) are kept; decide what else
   to strip; a lawyer's read of vendor licence terms on publishing measurements (plan section 10).
9. **Field hardening**: a stranger's Mac without Sean's ledger (maps / categories) - discovery today leans on the local map store for
   map_fp; first-run without it, sleep / lid-close handling in "overnight", disk-space checks before measuring.
