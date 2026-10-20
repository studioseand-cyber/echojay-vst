# FOR SEAN, 20 Oct 2026 (Kathy). FINAL. Nothing here was sent.

## 1. The trust files: `trust_levels.json`, `hold_states.json`
Copy to B's server as before: `comp_profiles/trust_levels.json` and `comp_profiles/hold_states.json`.
- Both are plain arrays in B's format. `validate_b.py` gives **0 errors**.
- They **replace** the 19 Oct files. `FOR_SEAN_19oct` is unchanged.

| format | certified | mapped | unsure | testable | can't test on this Mac |
|---|---|---|---|---|---|
| AU | 164 | 196 | 247 | 607 | 201 |
| VST3 | 7 | 31 | 81 | 119 | 196 |
| **all** | **171** | **227** | **328** | **726** | **397** |

**What changed from 19 Oct (162 / 236 / 334 of 732, held 391): +9 certified, -9 mapped, -6 unsure into held.**
- **+2: MAutoDynamicEq, AU and its VST3 twin.** EQ bands that measured cleanly once their own On switch was on now count; before, the v1 EQ certification skipped them.
- **+7 AU, re-measured this morning with Kathy's EQ rulings:**
  - **H-EQ (m), H-EQ (s), H-EQ-Light (m), H-EQ-Light (s):** 18 of 20 acceptance writes each (12 before).
    - Their HF shelf moves its corner with gain, so it is now mapped at -6 / -3 / +3 / +6 dB and **certified per gain: HF at -3 and +3 dB**.
    - LF is not certified yet: 2 of 4 writes miss by 7-8 %; its per-gain maps have few corners.
    - HMF, LMF and MF are certified.
  - **REQ 2 (m), REQ 4 (m), REQ 6 (m):** every band, every write (8/8, 16/16, 24/24). Their frequency labels are bare numbers ("173") on a control that declares Hz, which now counts, and every one matches the measurement.
    - **Band 1 is certified as a Bell, but REQ opens Band 1 as a Hi-Pass.** Kathy's rule: EchoJay never changes a band's type unless the user asks. The draft marks it (`shape_changed`, `type_default: "Hi-Pass"`, `type_measured: "Bell"`). Please use Band 1's maps only when the user's Band 1 is already Bell, or they ask for it.
- **The 19 Oct run** (a second full pass) was read in: 111 more controls read back, no level moved by it.
- **The 20 Oct run** (the "probes closed" build; a third pass) was read in.
  - It added no new read-backs and moved no level.
  - **Held +6:** kHs Bitcrush, kHs Dynamics and TH-U Slate (AU and VST3) showed a licence window this time, so they now count as held for licence, out of the totals.
- **Controls:**
  - the same count without this morning's re-measure gives 164 / 234 / 334: the +7 is the re-measure, the +2 is the code;
  - without the 20 Oct run it gives 171 / 227 / 334, held 391.

## 2. `EJMAP_EQ_CERTIFIED_20oct.zip`: the 7 EQ drafts behind the new certified rows
**Load these only after B accepts the new fields** in `NOTE_FOR_B_engage_and_type.md`:
- `freq_unit`;
- `freq_by_gain`;
- `type_control` / `type_default` / `type_measured` / `shape_changed`;
- `shape_changed_bands`.

Until then, the 7 rows are certified on our side but have no profile on yours.

## 3. `NOTE_FOR_B_engage_and_type.md`: a proposal for B (draft, not sent)
- `needs_engage` becomes a field, `engage: {switch, value}`, so a band's verdict can still say `level_dependent` / `stepped`.
- The type rule above.
- The new draft fields.

## 4. `ratio_law_sean_rescored_20oct.zip`: Sean's ratio-law run, salvaged, plus Kathy's 7
- **The +3 dB "MOVED" on every setting was our bug** (your curves are sine RMS; we read them as peak).
- Re-scored, every plugin whose measured setting was read back matches your served curve within 0.13 dB.
- 26 of the 27 now have per-setting curves.
- **Top-up: 8 plugins, about half an hour.** The list is inside, run with the fixed pack. **Open SSL Native Bus Compressor 2 once in Logic first** (it stopped on a PACE window).
- Its README says what B may load.

**How it was built** (`how_built/`):
- `recount_20oct.sh`: the 19 Oct recount plus the 19 Oct and 20 Oct runs and their AU holds (`held_19oct_au.json`, `held_20oct_au.json`). This morning's 9-EQ re-measure was added as an extra cert folder.
- `make_b.py`, `validate_b.py`.

Nothing was loaded for the count.
