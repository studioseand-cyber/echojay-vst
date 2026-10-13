# EchoJay Wizard: spec v1 (Kathy's design, 11 Oct 2026)

Branch `feat/ejmap-wizard`. The window app is built on the existing wizard engine. Every server part is **stubbed**, and nothing leaves the Mac. Section 6 is what Sean builds.

## 1. What it is

The wizard is a separate Mac app, downloaded from the EchoJay website. It has nothing to do with the plugin: it is not installed with it and does not run inside a DAW.

It has a very simple wizard look: a plain white window, the EchoJay logo, and one step on screen at a time. Each step has one short title, one or two sentences, and one button.

It measures the user's plugins that EchoJay's database does not have yet. It sends the results to EchoJay and rewards the user with a free month.

## 2. The screens (13 Oct: consent, eligibility, once only, the beta build)

**Normal build** (stubbed server):

| # | screen | what the user sees | what happens |
|---|---|---|---|
| 1 | **Welcome + sign in** | "Help EchoJay learn your plugins", email + password, **Sign in** | Sign-in (6.1) links the results and the reward to the account |
| 2 | **Consent** | "Before we start", the paragraph in 2b, an **I agree** tick, **Continue** | Continue stays disabled until the tick is set. The consent and its time go in the app's log. Nothing is scanned before it |
| 3 | **Scan** | "Checking your plugins", a Logic start-up style bar, each plugin's name as it is checked, "n of N" | Every installed plugin is mapped and checked against the database (6.2). A demo version is found here (3b) and skipped |
| - | **Already taken part** | "You've already taken part - thank you", **Done** | When `/lookup` returns `already_contributed: true` (6.5). Nothing runs |
| 3b | **Scan done: eligible** | "You have N plugins we don't have yet", "Help map them and get a free month. About X.", **Start** | When `/lookup` returns `eligible: true` (6.5) |
| 3b | **Scan done: not eligible** | "Thanks - your plugins are already mapped", "So there's nothing to do right now.", **Done**, and **Contribute anyway (no reward)** when there is anything to measure | Done quits; nothing runs. Contribute anyway measures and uploads with no reward and no promo screen |
| 4 | **Measuring** | "Measuring", the bar, the current plugin's name, "about X left" | As before. Closing the window or Ctrl-C: "Stopped - it carries on next time" (3a) |
| - | **Crash recovery** (on the next launch) | "Welcome back. The wizard closed while checking <plugin> - we've skipped it and carried on.", **Carry on** | The plugin goes on the never-retry list. Its crash report travels with the upload |
| 5 | **Upload** | "Sending your results", a short bar | The contribution bundle is sent automatically (6.3) |
| 6 | **Reward** | "Thank you - here's your free month", the code, **Copy code**, **Done** | Only when the server granted it on the ACCEPTED results (6.4, 6.5) |
| 6 | **No code** | "Thank you - your results arrived", the server's plain message (e.g. "The free month needs at least 5 new plugins that pass our checks; this time there were 3."), **Done** | When the receipt's `reward_eligible` is false |

**Beta build** (target `ejmap-wizard-beta`, `EJ_WIZARD_BETA`):
- **No sign-in and no promo.** The order is Welcome ("Thank you for testing the EchoJay Wizard", **Continue**), Consent, Scan, Scan done, Measuring, **Results saved**.
- **Scan done** in beta says "You have N plugins we don't have yet - Thank you for helping map them", with no reward. If there is nothing to measure: "Thanks - your plugins are already mapped".
- **Results saved:** "Thank you - your results are ready", "Saved as EchoJay-Wizard-results-<YYYY-MM-DD>.zip on your Desktop. Email this file to <beta address>.", **Show in Finder**, **Done**.
  - The file is the contribution bundle, unchanged. A second file the same day gets -2, -3 and so on; nothing is overwritten.
  - The address is the build constant `EJ_WIZARD_BETA_EMAIL`: **admin@signaturestudiosonline.com** (Kathy, 13 Oct).
- **Once only is not enforced in the beta.** It has no server, and the stub only records an accepted upload, which the beta never makes.

### 2b. The consent (wording for Kathy to approve)

> The wizard measures how your audio plugins respond to test tones and sends EchoJay the results: each plugin's name, maker, version, its list of controls and the measurements. It never sends audio, your files, your projects, your name, your computer's name or folder paths. The results go to EchoJay's servers, linked to your EchoJay account, and are used only to teach EchoJay how plugins behave. To make sure each Mac takes part once, the wizard also sends a one-way code made from this Mac's hardware ID - never the ID itself.
>
> ☐ I agree

In the beta the last sentence reads: "In this beta the results are saved as one file on your Desktop for you to email to us; nothing is sent by the app." (`wizapp::consentText`, pin WZ-FLOW-1.)

What it says is true of the bundle as built:
- **Profiles and drafts:** maps of control names, ranges and measured figures.
- **Crash reports:** redacted of the home folder, user name and computer name. One that still contains any of these is withheld.
- **No credentials, no paths:** the bundle check refuses to pack if any appear (section 3).
- **The device hash** (6.5) is a one-way hash, not a personal detail.
- **The device hash (Kathy, 13 Oct):** the release paragraph names it in one plain sentence (above); the beta's does not, because it sends nothing.

## 3. Behaviour rules

**3a. The stop (13 Oct).** Closing the window or a Ctrl-C stops the same way while scanning, checking or measuring: "Stopped - it carries on next time".
- What finished stays: maps, signature reads, demo checks, measured rows.
- What was in flight is discarded, never filed as crashed. No crash marker is left, so the next launch puts nobody on the never-retry list.
- A stopped scan is not a finished scan (pins WZ-STOP-1/2).

**3b. A stranger's Mac (13 Oct).**
- **No licence file:** a demo is found by what it does. Before measuring, each plugin gets a steady 997 Hz tone at -18 dBFS for 60 s, rendered offline in about 1 s.
  - Output that drops 20 dB below its own median and comes back is a demo's mute. That plugin is filed `demo`: not measured, not sent ("N plugins are demo versions and were skipped").
  - Measured here: three Melda demos drop out from 19-25 s in; 11 licensed plugins of every measured category never do.
- **Licence and iLok windows** are closed by the window watch, never clicked, and the plugin is filed needs_licence, quietly.
- **Disk:**
  - It refuses to start below 3 GB free, says what it needs (a run writes about 5 MB per plugin), and offers **Quit**.
  - It stops cleanly below 1 GB while measuring, in plain words, and carries on next time.

- **Where it opens.** The app keeps no session. The files in its work folder say where the user was, and it opens there (`wizapp::openOn`, pin WZ-APP-3):
  - not signed in → Welcome
  - a crash → Crash recovery (said before anything carries on)
  - rewarded → Reward
  - sent → Reward
  - measured → Upload
  - measuring started → Measuring (carries on)
  - scanned → Scan done
  - otherwise → Scan
- **Closing pauses.** The close button, or quitting, sends SIGINT to the engine's process group. That is Phase B's clean stop: no new plugin starts, and the plugins in flight are dropped and run again next time. The app quits once the engine has stopped. Reopening opens on Measuring, and the bar starts at the share already done.
- **The crash marker** (pin WZ-APP-1):
  - Before the engine loads a plugin (mapping, the quick check, measuring), it writes `<work>/engine/inflight/<plugin>.json`.
  - When that plugin's step ends, with any outcome, the file is removed. That includes a probe that crashed on its own, because the engine survives that and files `probe_crashed`.
  - A pause or the end removes all markers.
  - So a marker found when the app opens means the wizard itself went down with that plugin loaded.
- **Recovery.** For each leftover marker:
  - add the plugin to `<work>/skip.json`, the never-retry list (once);
  - write `<work>/crash_reports/<plugin>.crash.txt`. This is the newest EchoJayProbe / ejmap crash report written since the plugin was loaded, redacted: home path → `~`, user name → `<user>`, computer name → `<host>`, any `/Users/` → `/<users>/`. If there is no such report, the file says so;
  - remove the marker.
  
  The engine never loads a plugin on that list again, in any stage. The bundle carries the reports. One that still trips the privacy check is withheld, with a note saying so.
- **Two at a time.** With `--jobs 2`, two plugins can be in flight when the wizard goes down. Both markers remain and both are skipped. The screen names both. **[Open decision]** Should the second one be re-run alone once, and skipped only if it takes the wizard down again?
- **Skip what the database has** (pin WZ-APP-2): rule A never measures. Rule B measures only when its quick check failed. Rule C always measures.
- **No plugin left behind** (12 Oct, `EjmapWizardRoute.h`, pins WZ-ROUTE-1..4). The order per plugin puts the cheap answers first:
  1. **The database**, with no load. Rule A or pending_review is skipped entirely. Rule B gets only its quick check.
  2. **The server's category.**
  3. **The name rules.**
  4. **Out of scope by type or words**, not loaded.
  5. **EJ Map's signature reads** (the categorise step's reads, shared code), only for what is left. They are cached per plugin_id, so the app's second engine run loads nothing again. Then:
     - settled `eq` → the EQ queue;
     - settled `dynamics` → the compressor queue (the compressor certification decides; a gate or rider comes back "could not be measured" and is still reported);
     - another settled category → sent with its category;
     - unsettled, or a licence window → `needs_category`.

  Nothing is dropped silently. The summary says "N plugins we couldn't identify were sent for review", and names what was sent with a category, what was only listed, and what someone else already sent.
- **The loads ledger:** every load (map, quick check, signature reads, measuring) is written to `<work>/engine/loads.log` (time, stage, plugin). It is the record that rule A and pending_review plugins were never loaded.
- **Keep awake.** While the engine runs, the app holds an IOKit `PreventUserIdleSystemSleep` assertion named "EchoJay Wizard is measuring your plugins". It is released when the engine ends or is paused.
- **Nothing personal leaves.** The bundle holds profiles, tone-check sidecars and EQ profiles, a manifest, and the redacted crash reports. Every file is checked for the home path, user name, host name and credential-like fields before packing; a hit refuses the bundle. The account token is never in the bundle.

## 4. The architecture (as built)

```
EchoJay Wizard.app/Contents/MacOS/
  EchoJay Wizard     the window (JUCE GUI, tools/ejmap/wizard_app/WizardApp.cpp)
  ejmap              the engine: ejmap --wizard (EJ Map's code, same codebase)
  EchoJayProbe       the out-of-process plugin host (Developer ID signed, its own signature kept)
```

- The window starts the engine with `posix_spawn` in its own process group, with stdout going to `<work>/engine_output.txt`. It reads `<work>/engine/progress.json` (`ej_wizard_progress/0`) every 250 ms.
  - The Scan bar is the engine's Scanning + Checking (0–15 of its bar), shown as 0–100%.
  - The Measuring bar is the engine's Measuring band (15–97), shown as 0–100%. The engine's own rescan shows as "Getting ready".
- Engine calls:
  - scan: `ejmap --wizard --db <db> --out <work>/engine --ejmap-ledger <work>/store --jobs N --skip <work>/skip.json`
  - measure: the same plus `--measure --bundle`
- The work folder is `~/Library/Application Support/EchoJay Wizard/`, or `--work` for rehearsals. It holds `account.json`, `app_state.json`, `skip.json`, `crash_reports/`, `engine/`, `store/` (the wizard's own maps; it never writes `~/Library/ejmap`) and `app.log`.
- To assemble: `tools/ejmap/wizard_app/assemble.sh <build dir> <out>` copies the app, the engine and the signed probe into the bundle, signs the engine ad hoc, and re-seals the bundle ad hoc **without** `--deep` (so the probe keeps its signature).
- Rehearsal-only flags: `--auto` (no hands), `--auto-close-after S` (the close button), `--snapshots DIR` (a PNG of every screen), `--only NAME`, `--db`, `--store`, `--reports`.

## 5. Categories (12 Oct: all of EJ Map's)

The wizard measures every category EJ Map certifies. It uses **EJ Map's own Phase B category** for each: the same mode, outcomes, acceptance writes and serial lane (UAD, licence-governed, PACE, past trouble) as the certification. Only the work list differs: it comes from the wizard's queue (`<work>/engine/wizard_queue_<category>.txt`), not from the cert catalogue.

| category (server / signature) | measured by (Phase B) | starting estimate per plugin |
|---|---|---|
| compressor (and signature "dynamics") | `wizard`: the compressor certification + tone check | 340 s |
| eq | `wizard_eq` | 36 s |
| de-esser | `deesser` | 18 s |
| limiter | `limiter`, then `limitercomp` (the compressor certification on a limiter, Kathy 10 Oct) | 28 s + 340 s |
| saturation, amp_sim | `saturation` | 11 s |
| gate / transient_shaper | `gate` / `transient` | 17 s / 22 s |
| multiband | **not measured for now** (no server category): listed in the manifest's `held[]` with its map | - |
| pitch (tuners) | `tuners` | 111 s |
| reverb / delay | `reverb` / `delay` | 41 s / 45 s |
| channel_strip | `strips` (each section through its category) | 1200 s (no measured row yet) |
| modulation, meters, utilities, instruments | internal labels: out of scope, listed by identity only (never sent as categories) | - |

**The estimates** are the medians of Sean's own "ok" Phase B rows per category (his 4–11 Oct nights). They are replaced by this Mac's measured times as plugins finish.

**The order**: compressors → EQ → de-essers → limiters → (the limiter's compressor pass, only after an ok limiter row) → saturation → gates / transients → tuners → reverb / delay → strips. Each group is one Phase B pass (`--jobs` inside it), so a user who stops early has already given the most valuable results. A stop ends the order there; the next run carries on from the rows that landed.

**Ready to send:**
- compressor: its certification exported a profile and the tone check passed.
- EQ: a band measured and every acceptance write that ran passed.
- every other category: the category's row is `ok` and its cert draft was derived (`--phaseb-drafts`, the same rules). The draft (or drafts) goes into `profiles/` with `kind` and `measured_by`. A limiter also records its compressor pass's outcome beside it.

Everything else is reported with the cert's own outcome (needs a licence, passes no sound, crashed, won't load, could not be measured).

## 5b. The server's categories (Sean, 12 Oct) and the boundary table

The server has **12 categories**, lowercase, in Sean's spellings: `compressor, limiter, eq, de-esser, delay, reverb, saturation, gate, transient_shaper, channel_strip, amp_sim, pitch`.

EJ Map keeps its own internal names. The wizard translates **at the server boundary only**, with one table (`wizserver::toServer`, pins WZ-SRV-2 and 2b). `/lookup`'s category is read in the server's names, and each translates to itself.

| internal (EJ Map / wizard) | sent as | how it is measured |
|---|---|---|
| compressor, `wizard` | compressor | the compressor certification + tone check |
| limiter, `limitercomp` | limiter | the limiter mode; then the compressor pass, only after an ok limiter row |
| eq, `wizard_eq` | eq | the EQ certification |
| deesser | **de-esser** | the de-esser mode |
| delay / reverb | delay / reverb | the reverb-delay mode |
| saturation, when the record read an **amp sim's cabinet response** | **amp_sim** | the saturation mode |
| saturation, otherwise | saturation | the saturation mode |
| gate | gate | the dynamics mode (gate) |
| transient | **transient_shaper** | the dynamics mode (transient) |
| strips | **channel_strip** | the strip mode (each section through its category) |
| tuners | **pitch** | the tuner mode |
| multiband | — (internal) | **not measured by the wizard for now**: listed in `held[]` ("held: no server category") with its map. EJ Map on Sean's Mac is unchanged |
| modulation, meter, utility, instrument, midi, the signature's "dynamics" | — (internal) | out of scope, listed by identity only. A needs_category leaning that is not one of the 12 is sent as null |

**Validation:** every category an outgoing manifest carries must be one of the 12, or nothing is packed (`wizserver::invalidOutgoing`, pin WZ-SRV-3). That covers `profiles[].kind`, `other_categories[].category`, a non-null `needs_category[].leaning`, and no category at all on `held[]` / `out_of_scope[]`. Draft files keep EJ Map's internal names inside; only the manifest is the boundary.

## 6. Server endpoints (Sean's side; stubbed in the app)

All are JSON over HTTPS under **`https://www.echojay.ai/api/wizard/v1/`** (Sean, 12 Oct: not api.echojay.ai; `wizserver::baseUrl()`, pin WZ-SRV-1). Authenticated calls carry `Authorization: Bearer <token>`. Errors use HTTP status codes with a body of `{"error": "<code>", "message": "<plain words>"}`. Every call must be idempotent where noted, because the app retries after a pause or a crash.

### 6.1 Sign in

`POST /sign-in`
```json
{ "email": "user@example.com", "password": "…", "client": { "app": "EchoJay Wizard", "version": "0.1.0", "platform": "macOS 15.6" } }
```
**200:**
```json
{ "account_id": "acct_…", "display_name": "Kathy", "email": "user@example.com", "token": "…", "expires": "2026-11-10T00:00:00Z" }
```
**401** `{"error":"bad_credentials"}` · **403** `{"error":"account_locked"}`

The token is stored in the macOS Keychain (**the stub writes account.json without a token**). A single sign-on with the website session is optional later.

**Stub:** any email containing "@" and a non-empty password. `account_id = acct_stub_` + the first 10 hex characters of SHA-256(email).

### 6.2 Which of these do you already have (the lookup)

`POST /lookup` (authenticated), up to 2000 plugins per call. It is sent **before anything is loaded** (12 Oct), so `map_fp` and `param_count` are present only when the wizard already mapped the plugin on an earlier run:
```json
{ "plugins": [ { "plugin_id": "AudioUnit|6f747363|1.0.3", "name": "Lindell SBC", "manufacturer": "Plugin Alliance",
                 "version": "1.0.3", "type": "aufx", "map_fp": "afdb3916…", "param_count": 22 } ] }
```
**200**, one entry per plugin, in order:
```json
{ "results": [
  { "plugin_id": "AudioUnit|6f747363|1.0.3", "rule": "A", "category": "compressor" },
  { "plugin_id": "AudioUnit|7e8ca3f4|1.8.1", "rule": "B", "category": "compressor", "from_version": "1.8.0",
    "readback": { "writes": [ { "index": 3, "control": "Threshold", "norm": 0.5, "expect": "-10.0 dB" } ] } },
  { "plugin_id": "AudioUnit|49696d78|1.15.1", "rule": "C", "category": "compressor" },
  { "plugin_id": "AudioUnit|615c4d43|14.16.0", "rule": "C", "category": "modulation" },
  { "plugin_id": "AudioUnit|615d426d|14.16.0", "rule": "C", "category": null },
  { "plugin_id": "AudioUnit|61526042|14.16.0", "rule": "pending_review", "category": null, "pending_since": "2026-10-12" } ],
  "categories_version": "2026-10-11" }
```
- `rule`:
  - **A** = have it. Skipped entirely: not loaded, not categorised, nothing uploaded. A matches on plugin_id (uid + version); `map_fp` is compared only when the request carries one.
  - **B** = have another version; the client reads back `readback.writes` (no audio) and measures only if a display differs.
  - **C** = not in the database.
  - **pending_review** (new, 12 Oct) = another user already sent it as `needs_category` and Sean has not labelled it yet. Skipped entirely: not loaded, no signature reads, **not re-sent**.
- `category` (new, 12 Oct): the **Sean-approved** category whenever the server has one, always one of the server's 12 (section 5b): `compressor, limiter, eq, de-esser, delay, reverb, saturation, gate, transient_shaper, channel_strip, amp_sim, pitch`. That includes rule C plugins with no profile yet. `null` = the server does not know.
  - The wizard uses it FIRST. A category EJ Map certifies (section 5) goes to its measuring queue. Any other category (modulation, meter, …): the plugin is mapped and sent with its category (6.3 `other_categories`), not measured.
  - Without a server category, the wizard tries, in order: its name rules (compressor / EQ words); out of scope by AU type or meter / utility words (listed, never loaded); and only then EJ Map's signature reads on the user's Mac.
- The server decides A / B / C / pending_review and the category. The client never decides from its own copy of the database.
- `reference` (new, 12 Oct): the plugin's agreed **opening state** at this version, when the server has one: `{block: [[index, norm, name, text], …], agreeing: N, sources: […]}`. It is trusted when **2 or more independent Macs** opened identically. The AU's own factory preset was validated and rejected: it matches only 47%.
  - Before measuring, the client compares the plugin's opening block with it.
  - Same → measure.
  - Differs from a trusted reference → write every differing control back (recorded as `custom_default_restored`), read back, then measure under those writes.
  - Differs from a reference only one Mac vouches for, or the read-back fails → `custom_default`: not measured, **held** (listed in `held[]` with `held_reason: "custom_default"`) until a third source settles it.
  - **Every check casts a vote** (Kathy, 12 Oct): identical to the reference = agrees. A plugin identical to a single-source reference is the second source: it is measured, and the upload marks the reference as agreed. A disagreeing vote carries this Mac's opening block, so a third source can settle it. The votes travel in the manifest's `reference_votes[]` (6.3).
  - **Stub:** the database folder's `references/`.
- **Known multibands** (12 Oct): the client ships EJ Map's list (the 17 products Sean's cert found multiband, by uid). These are held at once, before any category or signature read.
- **Stub:** the engine's `--db` folder: `ej_comp_profile/1` + tone checks, `ej_eq_profile/1`, `categories.json` (the category, keyed by plugin_id or name) and `pending_review.json` (`[{"plugin_id": …}]`), with the same rules applied locally (`wizroute::ask`).

### 6.3 Upload

**Two paths** (Sean, 12 Oct; `wizserver::uploadPathFor`, pin WZ-SRV-4):
- **Direct**: the zip + the manifest + 64 KB of multipart headroom fit Vercel's 4.5 MB request body (4,500,000 bytes).
- **Signed blob**: anything bigger.
  1. `POST /contributions/upload-url` `{bytes, bundle_sha256}` → `{upload_url, blob_ref, expires}`
  2. `PUT <upload_url>` with the zip (`Content-Type: application/zip`)
  3. `POST /contributions/complete` `{blob_ref, bundle_sha256, manifest}` → the same receipt as the direct path

The stub's "signed URL" is a local folder (`<work>/server_stub/signed_blob/`), and its receipts carry `path: direct | signed_blob`.

**Direct:** `POST /contributions` (authenticated, `multipart/form-data`), idempotent on `bundle_sha256`:
- `manifest`: `manifest.json` (`ej_contribution/0`). It contains:
  - `profiles[]`;
  - `confirmations[]` (rule B passes);
  - `skipped[]` (plain reasons; never rule A or pending_review);
  - `crashed[]` (product, stage, crash_report path);
  - **`needs_category[]`** (new, 12 Oct): `{product, plugin_id, map_fp, leaning, file}` for each plugin the server, the name and the signature reads could not settle. Its file `needs_category/<product>.json` holds the wizard's map (identity + parameter list), EJ Map's signature reads (`level_dependence_db`, `frequency_span_db`, `rt60_s`, `repeats`, THD at -30 / -6, sideband, pitch move, notes), the leaning (a category or null) and the why. Sean labels it; the server then returns it as `pending_review` to other users until the label is in;
  - **`other_categories[]`** (new): `{product, plugin_id, map_fp, category, file}` for a category the wizard cannot measure yet. The file holds the map and the category;
  - **`out_of_scope[]`** (new): `{product, plugin_id, why}`. Instruments, MIDI, meters, utilities and modulation, listed by identity only (never loaded; the internal label stays local);
  - **`held[]`** (12 Oct): `{product, plugin_id, map_fp, map, why: "held: no server category"}`. Multibands, until the server has a category for them. Also `{product, plugin_id, map_fp, held_reason: "custom_default", why}`: a plugin that opened differently from its reference and was not measured;
  - **`reference_votes[]`** (12 Oct): `{product, plugin_id, reference: "<stem>.reference.json", agrees, agreeing_before, makes_trusted, measured_here, opening_block?}`. One for every plugin checked against a reference, whatever its state. `opening_block` (`[[index, norm, name, text], …]`) is present only when the vote disagrees. The server adds each agreeing vote to the reference's count; at 2 the reference is trusted. It keeps each disagreeing block for a third source;
  - `tool`, `platform` and `created`.
- `bundle`: `contribution.zip` (the manifest, `profiles/`, `crash_reports/`, `needs_category/`, `other_categories/`).
- `bundle_sha256`: a hex string.

**200:**
```json
{ "receipt_id": "rcpt_…", "genuine": true, "checked_in_ms": 2400,
  "profiles_received": 3, "confirmations_received": 2, "crashed_reported": 1,
  "needs_category_received": 2, "other_categories_received": 1, "out_of_scope_received": 1,
  "held_for_review": [ { "product": "…", "why": "…" } ],
  "reference_votes_received": 2, "references_now_agreed": [ "AudioUnit_…_1.0.reference.json" ] }
```
`references_now_agreed` names each reference this upload took to 2 agreeing Macs. It names a reference only once: later votes count but are not announced again.
- **409** `{"error":"duplicate", "receipt_id":"rcpt_…"}` (the same bundle again: return the first receipt).
- **422** `{"error":"not_genuine", "message":"…"}`.
- **413** if over 50 MB.

**Genuine** means: the schema is valid; the bundle's own probe cdhash is on the allow-list; each profile's `map_fp` matches its plugin_id and param_count; each tone check passes at g 2; the timestamps are plausible; there are no personal fields. It must take seconds, not a measurement. Publishing is a later, separate step (holding area → agreement → publish).

**Stub:** copies the zip to `<work>/server_stub/uploads/<receipt>.zip`. Genuine = the manifest schema only. Votes: each one is appended to `server_stub/reference_votes.json`. A tally per reference is kept in `server_stub/references/<file>`: `agreeing` (starting from the vote's `agreeing_before`), `trusted`, and the `disagreeing` blocks (`stub::recordVotes`, pin WZ-CD-2).

### 6.4 Promo code

`POST /promo` (authenticated), idempotent per account:
```json
{ "receipt_id": "rcpt_…" }
```
**200:**
```json
{ "code": "EJ-FREE-8E7AE524", "issued_now": true, "months": 1, "emailed_to": "user@example.com", "expires": "2027-01-31" }
```
- A second call for the same account returns the same code with `"issued_now": false`.
- **403** `{"error":"receipt_not_genuine"}`.
- **409** `{"error":"no_new_profiles"}`, only if Kathy rules "no code when nothing new".

The server emails the code (the stub writes `<work>/server_stub/outbox/<account>.txt`).

**Stub:** `EJ-FREE-` + the first 8 hex characters of SHA-256(account_id + "|wizard-v1"), uppercase. One per account in `server_stub/promo_ledger.json`.

### 6.5 Eligibility, once only and the reward (Kathy, 13 Oct) - the server decides, the app only shows it

**`/lookup` adds** `{"eligible": bool, "count": N, "minimum": M, "already_contributed": bool}`.
- **The count:** NEW CONTRIBUTABLE plugins, which means:
  - rule C (to be measured);
  - not `pending_review`;
  - a server category the wizard measures (not held multiband, not out of scope);
  - not licence-blocked and not a demo.
- **Eligible:** `count >= minimum` and not `already_contributed`.
- **The minimum:** server-configurable, default 5 (confirmed by Kathy, 13 Oct).
- **Stub:** `stub::lookupEligibility`, counting the scan's `queued` rows (pins WZ-ELIG-2).

**`/contributions` decides the grant on the ACCEPTED results, never on the queue or the client's count.** The receipt carries `accepted_new`, `reward_minimum`, `reward_eligible` and, when no code is granted, `reward_message`.
- **What counts:** each profile whose evidence is in the bundle:
  - a compressor's tone check passed within 0.5 dB;
  - an EQ's acceptance;
  - a draft that is present.
- **What doesn't count:** a duplicate plugin_id in the upload, or a plugin the server already knows (its database, or an earlier accepted upload).
- **Below the minimum:** no code, and a plain message.
- **Stub:** it opens the stored zip and ignores any count in the manifest (pin WZ-ELIG-3).

**`/promo`** issues only for a receipt with `reward_eligible: true`. This replaces "a code even when nothing new". **Contribute anyway** uploads with no reward.

**Once only** (server-configurable `once_only`, default true - confirmed ON by Kathy, 13 Oct): one COMPLETED contribution per account AND per Mac.
- **Completed** = an upload the server accepted.
- **The same run:** a paused, crashed or resumed run is the same run and may finish. Lookups alone record nothing, so a scan that was not eligible does not use the go.
- **A second run** is refused at `/lookup` with `already_contributed: true`, which shows "You've already taken part - thank you", and nothing runs. That applies to a second run on the same account (any Mac), and on the same Mac with a new account.
- **Pins:** WZ-ONCE-1 and WZ-DEV.
- **Later:** `once_only: false` would allow repeat contributions. A repeat never earns a second reward, because the reward needs new accepted plugins and the account or Mac not rewarded before.

**Abuse limits (for Sean):**
- **One reward per account AND per Mac:** the server records both on the accepted upload.
- **The device hash** = SHA-256 of a fixed app salt (`echojay-wizard-device-v1|`) and the Mac's hardware UUID (`IOPlatformUUID`, upper-cased).
  - The raw UUID never leaves the Mac.
  - The salt makes the hash useless for matching other apps' identifiers.
  - It stays the same across reinstalls and accounts on that Mac.
  - It is sent with `/lookup` and `/contributions` as `device`.
- **Idempotent uploads** on `bundle_sha256`: the same bundle again returns the first receipt and grants nothing new.
- **The server never trusts the client's count:** it recomputes from the bundle and its own database.
- **Server-side only:** the minimum, `once_only`, and the accepted-count rule are configuration on the server; the app has no copy of them.

## 6b. SERVER-SIDE, awaiting Sean (the client has stubs only)

- **The account → mapper-token bridge:** sign-in returns the account. The server links it to the mapper token its map and profile intake uses. The stub signs any email and password and has no token.
- **The review / holding area:** an upload lands in holding, then agreement with another version or a second user, then publish. The stub's genuine check is the manifest schema only.
- **The reference votes** (12 Oct): count the agreeing votes per reference and plugin version, independent Macs only (one vote per account or machine, not per upload). Trust at 2. Keep the disagreeing blocks and settle on a third source. Return the reference in `/lookup`. The stub tallies per upload and does not de-duplicate machines.
- **The promo rules + issuing + email:** one code per account, the "nothing new" rule (Kathy: proposed yes), and the email. The stub writes a local ledger and an outbox file.

The app's sign-in, upload-to-review and reward screens work on these stubs.

## 7. Not in v1

These are tracked in WHATS_LEFT.md:
- Developer ID signing and notarisation of the app;
- Sean's endpoints;
- Windows;
- the legal and privacy review;
- categories beyond compressors and EQs;
- the Keychain;
- an in-app "send a problem report" for an app crash outside a plugin load (no marker is written then, so nothing is skipped, which is right, but nothing is reported either).
