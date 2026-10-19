# FOR SEAN, 20 Oct 2026 (Kathy). Nothing here was sent.

## The trust files, with the 19 Oct run added
Copy to B's server as before: `trust_levels.json` to `comp_profiles/trust_levels.json`, and `hold_states.json` to `comp_profiles/hold_states.json`.
- Both are plain arrays in B's format. `validate_b.py` gives **0 errors**.
- They **replace** the 19 Oct files. `FOR_SEAN_19oct` is unchanged.

| format | certified | mapped | unsure | testable | can't test on this Mac |
|---|---|---|---|---|---|
| AU | 157 | 203 | 250 | 610 | 198 |
| VST3 | 7 | 31 | 84 | 122 | 193 |
| **all** | **164** | **234** | **334** | **732** | **391** |

**What changed from 19 Oct (162 / 236 / 334 of 732, held 391):**
- **+2 certified, -2 mapped: MAutoDynamicEq, AU and its VST3 twin.** EJ Map now counts an EQ band that measured cleanly once its own On switch was on (its draft carries the switch as `engage`).
  - Before, such bands were skipped by the v1 EQ certification.
  - MAutoDynamicEq has 4 of them. Its dynamic band stays uncertified.
- **The 19 Oct run** (a second full pass over the same plugins) was read in: read-back credit rose by 111 controls and 4 rows' reasons changed, but no level moved. It confirmed the 18 Oct run.
- **The 9 EQs re-measured tonight** (H-EQ / H-EQ-Light m/s, LinEQ Broadband m/s, REQ 2/4/6) moved no level:
  - **REQ:** every band now measures and passes every acceptance write, but its frequency labels carry no "Hz" on the plugin (a decision is pending on our side);
  - **H-EQ's shelves:** they move their corner with gain;
  - **LinEQ:** two known items remain.

**How it was built** (`how_built/`):
- `recount_20oct.sh`: the 19 Oct recount plus the 19 Oct run's engine folder (as readings) and its AU hold rows (`held_19oct_au.json`).
- `make_b.py`, `validate_b.py`.

Nothing was loaded for the count.
- **Control:** the unchanged 19 Oct recount, run with tonight's build, gives 164 / 234 / 334. The +2 is tonight's EQ change, not the new data.
