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

## Left before it is a real app
1. **UI**: a window (scan list, "map now / overnight", progress, review-and-Send). Today a CLI; the JUCE app shell exists (MainComponent)
   but no wizard screen. Estimated hours / ETA from real times per category.
2. **Sign-in + upload**: per-user accounts and tokens (plan section 5) - server side (Sean): auth, the contribution intake (holding
   area, validation, agreement with another version or a second user, then publish), the reward ledger. The client's upload call,
   retry / resume of an interrupted upload, and the Send confirmation.
3. **The real database lookup**: "what's missing" on the server (plan section 8) instead of a local folder; the server, not the client,
   decides A / B / C, and records B confirmations and live-check misses.
4. **Categories beyond compressors and EQs**: de-esser / saturation / reverb / delay / transient / gate / limiter / tuner / multiband /
   strips need their profiles published and their lookups defined (the EQ pattern: the profile's written controls for the B check, the
   category's acceptance as the send gate). And a category for a plugin neither the catalogue nor its name places: today "not measured
   by the wizard yet" - the measurement categoriser (--categorise-propose) could decide it on the user's Mac.
5. **Licence handling for strangers**: the user's own licences.csv does not exist - the wizard relies on window / PACE detection and the
   serial lane; a "skip anything that asks for a licence" default and a plain explanation screen.
6. **Notarisation** (not just Developer ID signing) of the app and the probe; a first-run Accessibility prompt explained.
7. **Windows**: AUs are Mac-only - a VST3 probe + window watch for Windows, or a Mac-only first launch (plan section 9).
8. **Privacy review**: the manifest's `platform` (OS version) and the profiles' `measured.tool` (probe cdhash) are kept; decide what else
   to strip; a lawyer's read of vendor licence terms on publishing measurements (plan section 10).
9. **Field hardening**: the empty-store path is built and rehearsed; still open - sleep / lid-close handling in "overnight", disk-space
   checks before measuring, and the wizard's maps are parameter lists only (the GUI's role mapping is not run: the dial side must accept
   a map without roles, or the server supplies them).
10. **Run as the user, in the user's real home**: plugins find their activation under the user's home folder. Rehearsing with HOME pointed
   at an empty folder made every Plugin Alliance unit pass digital silence (4 of 4 filed silent_output, not sent - the filing worked). A
   sandboxed or relocated app would do the same: the app must keep the user's real home and Application Support visible to the probe.
