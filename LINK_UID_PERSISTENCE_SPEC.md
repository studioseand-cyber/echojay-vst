# LINK_UID_PERSISTENCE_SPEC — corrected: persistence exists; the subject was the claim guard

**Status: CORRECTED 25 Aug 2026. The original draft proposed building uid
persistence. Persistence ALREADY EXISTS and predates the draft:
`getStateInformation` saves `instanceUid_` and `setStateInformation`
restores it (LinkProcessor.cpp), so a reopened project returns with its
saved identity. The evidence that exposed the error: rack-b3999c060d.json
read rev 10 / 3 slots one day and rev 5 the next — a returning instance
whose process-local revision counter restarted under the same uid.**

## 1. The real defect, and its fix (BUILT, 25 Aug 2026, by ruling)

The uid churn attributed to "per-launch uids" was the CLAIM-TIME COLLISION
GUARD: it re-minted whenever any `inUse` registry slot carried the saved
uid — and `inUse` cannot tell a live duplicate from a ghost. Every unclean
kill left a frozen slot holding the uid; the relaunch saw a "collision",
re-minted, and burned the identity — orphaning that uid's files and its
structplan journal. The churn engine, not a design property.

The guard now decides by the holder's heartbeat (`LinkShm::UidClaimGate`,
pure, three arms, functionally gated):

- **Proven-live holder** (heartbeat observed climbing): a genuine duplicate
  — copy/paste clones the saved state, uid included. THIS instance
  re-mints; first-alive keeps the uid, the copy becomes a new channel.
- **Observed-frozen holder** (no climb through the threshold, ~5 claim-retry
  ticks): a ghost of a dead launch. The slot is reaped and the uid ADOPTED
  — the crash/kill relaunch keeps its identity, which is what makes the
  saved uid worth saving.
- **Undecided** (just-appeared holder, too few observations): WAIT,
  unregistered, retried on the ~1s tick. An unproven holder is never
  adopted — reaping a possibly-live slot is the one unrecoverable mistake.

## 2. What stable identity now holds, and its remaining limits

- Ghost selector rows: liveness-before-listing hides them; the fixed guard
  stops manufacturing new identities against them.
- File accumulation: one channel keeps one uid across relaunches; the
  reaper (narrowed, §3) tidies protocol transients only.
- Orphaned structplan journals: a crash relaunch now reclaims its uid, so
  `structplan-<uid>.json` IS found again. The KNOWN HOLE recorded in
  RACK_STRUCTURE_EDIT_SPEC.md narrows from "never survives relaunch" to
  the honest residue below.
- The five "Audio 1_04" rows: one channel, one row, henceforth.

Remaining limits, stated rather than wished away:

- **A stale uid on a genuinely different channel** is still possible: "save
  as", template projects, dragging a plugin between projects all put an old
  uid on what the user considers a new channel, and the registry cannot
  distinguish that from a legitimate return (both present a saved uid with
  no live holder). Files keyed to the uid then describe the old channel
  until overwritten — including, worst case, a journal "recovered" into
  the wrong rack. NOTE THE OPEN TENSION: the journal restore deliberately
  WINS over a divergent session snapshot (RACK_STRUCTURE_EDIT_SPEC
  amendment 2 — a mid-plan Cmd-S must not beat the pre-images), so
  divergence does NOT block a wrong-channel journal today; it only notes
  both truths. Distinguishing "divergent because mid-plan save" from
  "divergent because wrong channel" is unresolved; a generation counter on
  the uid is the escalation path if this ever bites in practice.
- **Instances never saved** still mint per-launch and churn on kill; only
  a saved project pins identity. Working as intended.

## 3. The reaper under the corrected premise

"A dead uid can never be addressed again" is FALSE — uids return. The
reaper is narrowed (same ruling) to what stays safe when they do:
protocol transients (lease, racklock, ctrl/chain cmd+ack — consumed or
recency-gated, recreated from nothing) and audio rings unreferenced by any
registry slot (recreated at openRing). `rack-*.json` sidecars are now
SPARED (a returning uid's rack description); `structplan-*.json` was
always spared (someone's rollback).

## 4. Out of scope, unchanged

Cross-machine identity; uid migration for existing sessions; any uid
scheme derived from stable host properties (track name/position are
user-mutable and collide across projects — the saved-state uid is the
right anchor).
