# Next round — the queue, in order

Written 10 Oct 2026. Nothing here is started; this is the agreed order for the round AFTER the
levelling-redesign merge batch.

## 1. RE-DERIVE THE GR-MODEL CALIBRATION LEGS — before any remote-control work

Sean's ruling (10 Oct): these are marked **known-stale and named** in `loudness_loop_guard`
(`kKnownStaleLegs`), excluded from the exit code so that any *other* red still fails the gate.
**They must be green before anything reaches Logic.**

The seven assertions, and why they are stale rather than broken:

| Leg | Symptom |
|---|---|
| `K5` ×4 | the GR estimate is compared against a "truth" computed around the old **Level OUT** tap. `K5` reads estimate `+4.67` against truth `−6.16` — a **sign** disagreement, so it is the leg's arithmetic, not a 1 dB tolerance |
| `K4` | the true-peak line's hits figure, same tap |
| `K1` (capped trim) | the capped trim came back `0.01` |
| `O1` | its premise is "the hits are typically **under** the 10 dB Commercial cap", but the estimate reads **11.7 dB** where the product's own `hitsMeasure()` reads **2.9 dB** |

`O1` is on the list for the same single cause as `K1`/`K4`/`K5`, not as a widening of scope: the
estimate and `hitsMeasure()` disagree about the same quantity in the same proposal.

What re-derivation means here: the tap moved to the limiter's **GAINED input**
(`limv2::MeterTap` / `inputLevels()`), so the span the estimate covers is no longer the span the
legs' truth covers. Derive the truth from the new tap, then prove each leg **both directions**.

## 2. Settings sync — from B's findings (10 Oct)

B's server **now merges**, which answers the question left open in
`Source/EchoJayAPI.cpp` (`saveUserSettings`) and in `ECHOJAY_API_CONTRACT.md` §6.

- [x] **Never POST `/api/data` after a failed GET.** *Already in* — the predicate
      `echojay::userDataWriteMayProceed` gates it, and it is executed by the suite rather than
      grepped for. No work owed; listed so the queue matches B's list.
- [ ] **Stop writing the four empty arrays.** `chats`, `albums`, `reviews`, `refTracks` are each
      written as `[]` when absent from an otherwise-good body. The code says, in a comment, that
      omitting the key is safe **only** under merge semantics and that merge-vs-replace was
      UNANSWERED. B has now answered it: **a missing key keeps the stored value, but an explicit
      empty array still clears it.** So omit the key when the GET did not carry it. Leg: a good
      body missing `albums` must produce a payload with **no** `albums` property.
- [ ] **Include `pinnedProjects` in the settings sync.** Not currently in the profile payload.
- [ ] *(optional)* **Send `baseUpdatedAt`** from the 200 response, for conflict detection.

## 3. Remote control, stages 1–5 + the `doOp` executor

Per `LINK_REMOTE_CONTROL_PLAN.md` and `docs/CONTRACT_LINK_COMMANDS.md`, merging A2's work as it
lands. **Blocked behind item 1.**

## 4. Owed leg (noted when leg N was retired)

The target stage is the **LAST** slot, so an insert after it changes which slot the loop drives.
New subject, so a new leg rather than a re-aim.

## Flagged, needing someone else's decision

- **Sean:** the merged tree now has **two** stores for reference sets — the kept preset feature
  and integration's new reference library.
- **B:** emit `option: "commercial"` rather than collapsing it to `pushed`;
  `docs/ECHOJAY_API_CONTRACT.md` does not state the current chat-stream refusal shape.
