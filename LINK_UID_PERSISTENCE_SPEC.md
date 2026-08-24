# LINK_UID_PERSISTENCE_SPEC — a Link identity that survives relaunch

**Status: SPEC ONLY (25 Aug 2026). Not built, by ruling. Companion to the
KNOWN HOLE recorded in RACK_STRUCTURE_EDIT_SPEC.md (journal orphaned by
per-launch uid) and to the liveness/reaper work that treats a dead uid as
permanently unaddressable.**

## 0. The problem in one paragraph

`instanceUid_` is minted fresh every launch. Everything keyed on it —
sidecars, leases, locks, ctrl/chain command files, structplan journals, and
registry row identity — therefore dies with the process and is reborn under
a new name on relaunch. The observed costs: ghost selector rows (a killed
launch's registry slot outlives it), 60+ dead uid-keyed files on one
machine, a crash-recovery journal that can never be found by the relaunched
Link that needs it, and five identically-named "Audio 1_04" rows — the same
DAW channel, five launches, five identities, no way to tell the live one
apart by name.

## 1. The proposal

The Link persists its uid in its own saved state (getState/setState), so a
reopened project reconnects THE SAME identity.

- **Mint once**: a Link with no saved uid mints one (today's path) and saves
  it with the next state snapshot. A Link restored WITH a saved uid adopts
  it instead of minting.
- **Adoption is claimed through the registry**, which stays the arbiter of
  liveness: adopt-then-register is one operation, and the collision rule
  below runs inside it.
- **Files keyed by uid become durable per CHANNEL, not per launch**: the
  sidecar, journal, and command files a relaunch finds under its own uid are
  its own history. The reaper's "dead uid can never be addressed again"
  premise inverts for persisted uids — see §4, this is the sharpest edge.

## 2. Collision handling (copy/paste duplication)

Copy/paste or track duplication clones the saved state, so TWO plugins carry
the same saved uid. The second to register must notice and re-mint:

- **Detection at claim time**: registering an adopted uid scans the registry
  first. If a PROVEN-LIVE slot (heartbeat climbing — the RegLiveness rule,
  not inUse alone) already carries that uid, this instance is the duplicate:
  it mints a fresh uid, registers under it, and saves the new uid at the
  next state snapshot. First to claim keeps the identity; the copy becomes a
  new channel, which matches user intent — a duplicated track is a new
  track.
- **A NON-proven slot with the same uid is a ghost of a dead launch**: reap
  it and adopt. Without this rule, the crashed-then-relaunched case — the
  whole point of persistence — would read as a collision and re-mint,
  orphaning the journal again.
- **The race** (two copies registering in the same tick, neither proven
  yet): both see no proven holder; both claim; the registry claim itself
  must arbitrate — first CAS on the slot wins the uid, the loser re-mints.
  The loser's files-under-the-shared-uid question is §4's stale-claim case,
  bounded to seconds by the immediate re-mint.
- **Same-uid, both saved, sessions opened on two machines**: no shared
  registry, no collision, no problem — the shared dir is per-machine.

## 3. What it fixes

- **Ghost selector rows**: a relaunch re-claims its own registered slot
  (same uid) instead of leaving the old row to the 30s reaper and adding a
  new one beside it. Liveness-before-listing already hides ghosts; with
  persistence they largely stop being CREATED.
- **Per-launch file accumulation**: one channel = one uid = one sidecar,
  one lease name, one journal path, forever. The reaper becomes a rare-event
  janitor (deleted tracks) instead of a per-launch necessity.
- **Orphaned journals**: `structplan-<uid>.json` written before a crash IS
  found by the relaunched Link — the KNOWN HOLE in
  RACK_STRUCTURE_EDIT_SPEC.md closes, and the phase-2 crash guarantee
  becomes what it was believed to be.
- **The five "Audio 1_04" entries**: one channel keeps one row. Display
  disambiguation stops depending on uid-suffixed untitled names.

## 4. What it risks

- **A stale uid claimed by a genuinely different channel.** The saved blob
  travels: "save as", template projects, dragging a plugin between
  projects, or restoring an old session over a new one all put an OLD uid
  on what the user considers a DIFFERENT channel. The registry cannot tell
  "same channel, relaunched" from "old blob, new channel" — both present a
  saved uid with no live holder. Consequences worth naming before building:
  - files keyed to that uid now mean something else — a journal written by
    the old channel could be "recovered" INTO the new one (restoring a rack
    the user never had there: worse than the orphan it fixes, because it is
    wrong loudly rather than missing quietly);
  - a main's cached rack for the uid describes the old channel until the
    new sidecar overwrites it.
  Mitigations to decide between at build time: journal adoption requires a
  base-identity match against the restored rack (the §3e machinery already
  exists for exactly this shape of question), and/or the uid carries a
  generation counter bumped on adoption so consumers can distinguish eras.
- **The reaper's premise inverts.** Today "not live + old = dead forever"
  is safe by construction. With persistence, a channel in a CLOSED project
  is not live for weeks and must not be reaped. The reaper must then key on
  something else (e.g., only files whose uid has been superseded by a
  re-mint, or nothing at all — accept accumulation bounded by real
  channels). This spec does not pick; it insists the reaper be revisited IN
  THE SAME CHANGE as persistence, not after the first lost sidecar.
- **Registry slot exhaustion semantics shift**: 16 slots, and persisted
  uids mean closed-project channels do not free their conceptual identity —
  but they DO free their slot (registration is still per-process). No
  change needed; noted so nobody "fixes" it.

## 5. Out of scope

Cross-machine identity, uid migration for existing sessions (they mint on
first save under this scheme and lose nothing they have today), and journal
adoption by identity-matching alone (an alternative that fixes ONLY the
journal hole; decided against pursuing separately so the identity question
is answered once, not twice).
