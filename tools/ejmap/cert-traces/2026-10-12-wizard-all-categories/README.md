# The wizard on every category: rehearsal, 12 Oct (feat/ejmap-wizard)

**Set-up**
- 13 plugins: one per category that is hostable here, plus Lindell SBC (already in the mock DB, so never loaded).
- The categories come from the stubbed `/lookup` (`categories.json`, Sean-approved).
- An empty wizard store and `--jobs 2`.
- Two engine runs, as the app does: `run1_scan.txt` (9 s), then `run2_measure_bundle.txt` (29 min 41 s).
- The real `~/Library/ejmap`: 1123 maps before and after, and 0 files changed.

## Time per category and what landed (in the measuring order)

| pass | category → Phase B | plugin | seconds here | Sean's median | result |
|---|---|---|---|---|---|
| 1 | compressor → wizard | elysia mpressor | 330.5 | (340) | **measured**: profile + tone check |
| 2 | eq → wizard_eq | MFreeformEqualizer | 4.0 | 63 | could not be measured: 0 bands measured |
| 3 | de-esser → deesser | SPL De-Esser Dual-Band | 3.0 | 18 | could not be measured: the mode found no threshold control by name (`nothing_measured`). **Sean's row for this plugin is the same**: a poor pick, its 3 s "ok" there was a no-op |
| 4 | limiter → limiter + limitercomp | bx_limiter | 8.0 + 351.2 | 28 / - | **measured**: limiter_ceiling draft + the compressor pass's exported profile and tone check |
| 5 | amp_sim → saturation | bx_rockrack V3 Player | 2.3 | 11 | **measured**: saturation_profile draft |
| 6 | gate → gate | Unfiltered Audio G8 | 86.2 | 17 | crashed: probe crashed mid-trace (2 of 214 traces). The wizard branch lacks cert's crash-position fix 4951cccd, which nudges G8's Hold off 0 ms |
| 6 | transient_shaper → transient | spiff | 325.5 | 22 | **measured**: transient_profile draft (25× Sean's median here) |
| 7 | multiband → multiband | OTT | 19.3 | 51 | **measured**: multiband_profile draft |
| 8 | pitch → tuners | bx_crispytuner | 129.1 | 111 | **measured**: tuner_profile draft |
| 9 | delay → delay | ADA STD-1 Stereo Tapped Delay | 42.9 | 45 | **measured**: space_profile draft |
| 9 | reverb → reverb | bx_rooMS | 139.2 | 41 | **measured**: space_profile draft |
| 10 | channel_strip → strips | Lindell 50 Channel | 380.2 | (1200 stated) | **measured**: strip_profile draft |

**Summary:** "1 plugin ready, 9 newly measured, ready to send, 1 crashed (we moved on), 2 could not be measured". The bundle holds 13 files: 9 drafts and profiles, the limiter's compressor pass, and the manifest (`bundle_files.txt`).

## The scan screen's estimate

The first run said 1159 s (19 min). Measuring took 29.7 min. That estimate divided all the work by `--jobs`, but the passes run one after another, and a pass with one plugin cannot use two jobs. It is now summed pass by pass, and a pass is at least its longest plugin (pin WZ-CAT-5). The same queue now estimates 2191 s (37 min). It errs on the long side because the strip's starting figure (1200 s, the cert's stated estimate) is three times the 380 s it took here. Measured times replace the figures as plugins finish.

## Found and fixed during the rehearsal

- **The limiter's compressor pass profile was not in the bundle.** It is sent like any compressor: exported, and the tone check passed (pin WZ-CAT-6).
- **The new read of the compressor pass's outcome dangled** (`getArray()` on a temporary). Held now. Verified by re-bundling a copy: `bx_limiter_1.15.2.json` and its tone check are in the zip, with no loads.

**Pins:** WZ-CAT-1..6, plus WZ-ROUTE-2/3 updated (a reverb is now measured). 7 mutants all red. Suite 4093 checks, 0 failures.
