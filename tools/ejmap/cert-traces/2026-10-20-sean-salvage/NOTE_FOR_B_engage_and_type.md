# Note for B: two changes to the EQ draft (proposal; DRAFT, not sent)

From Kathy and EJ Map, 20 Oct 2026. Nothing changes on our side until you say the server accepts it.

## 1. `needs_engage` becomes a field, not a verdict
**Today:** a band that only moves once its own On switch is on gets the verdict `needs_engage`, and the switch is in `engage` as text ("Band3 On/Off = 'On'").
- That verdict wins over `level_dependent` and `stepped`, so an engaged band hides them.
- Our own count and certification were skipping these bands (fixed on our side; REQ 2/4/6 read every band at -18 -> -18.0 dB).

**Proposal:**
```json
"verdict": "measured",                       // or level_dependent / stepped / dynamic ... from the measurement alone
"engage": { "switch": "Band3 On/Off", "value": "On", "norm": 1.0 }
```
- The verdict says what the measurement found.
- `engage` says what must be written first for the band to work.
- During a changeover, old drafts keep `"verdict": "needs_engage"` with `engage` as text. Could the server treat both forms the same until we re-send?

## 2. EchoJay never changes a band's type unless the user asks (Kathy's rule)
Some bands only respond to gain at another type: REQ's Band 1 instantiates as a Hi-Pass, and gain moves it only as a Bell. We measure such a band at that other type, but the map now says so, and the server should respect it.

**New fields on a band:**
```json
"type_control": "Band1 Type",
"type_default": "Hi-Pass",        // the type the plugin opens with
"type_measured": "Bell",          // present only when the band was measured at another type
"shape_changed": true             // present only then
```
- **Top level:** `"shape_changed_bands": ["Band1"]`.
- **The rule for the server:**
  - use a shape-changed band's maps only when the band is already at `type_measured` in the user's session, or the user asks for that type;
  - never write the type control on its own initiative;
  - `engage` never contains a type write.
- A band whose `type_default` is already gain-capable has no `shape_changed`, and nothing changes for it.

## 3. Also new in the draft (already in our files; please accept)
- **`freq_unit`:** the unit the plugin declares for the band's frequency control ("Hz").
  - Our v1 certification accepts a bare-number label ("173") as Hz only when this says Hz and the measured centre matches the label.
- **`freq_by_gain`:** for a shelf whose corner moves with gain, its frequency map taken at about -6, -3, +3 and +6 dB.
  ```json
  [{ "gain_db": 3.0, "gain_display": "3.0", "gain_norm": 0.58, "rows": [{ "norm": 0.0, "display": "22", "corner_hz": 152.0 }, ...] }, ...]
  ```
  - The server should place such a shelf's frequency with the map nearest the gain it is dialling.
  - The shelf is certified per gain (we certify only the gains whose acceptance writes all passed).
