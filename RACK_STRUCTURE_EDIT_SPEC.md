# Rack structure editing — spec

Written 22 Aug 2026 against `RACK_BORROW_IMPLEMENTATION_SPEC.md` (steps 1–3
built: borrowed host, solo, Apply & Release with per-slot commit filtering).
**A spec, not a build.** Apply stops being N independent state writes and
becomes an ordered PLAN — adds, removes, moves, plus the settings commits
that already work. Six questions answered with recommendations, none assumed.

---

## 0. The plan, as a thing

One document, not a burst of ctrl-cmds: Apply composes a single plan —
`{ baseIdentity, ops[], states{}, preImages{} }` — and sends it as one
command file. The Link journals the whole plan to disk BEFORE acting on any
of it, applies ops in order, and answers one ack with per-op results.
Reasons: atomicity (§1) requires the Link to see the whole plan before the
first mutation; a burst of independent commands can interleave with nothing;
and the journal is what makes the crash case survivable. Old Links never see
a plan: structure editing is behind a new additive capability announce
(`structureEditCapable` in the sidecar, absent = false), the same
never-half-engage pattern as `borrowCapable`.

## 1. Atomicity — all-or-nothing, argued

**Recommendation: all-or-nothing, with rollback from held pre-images, and a
journal-driven restore for the crash case. Not named-partial.**

The argument: §2's falsified-chain reasoning is *stronger* here than at the
pull. A refused pull costs nothing — nothing engaged. A partial Apply leaves
the LINK'S REAL RACK wrong — half the user's structure with half the old
one, in the place the user is no longer looking. Named-partial would be
honest words about a dishonest rack.

All-or-nothing is genuinely implementable because every op is reversible by
construction:

- Every settings commit's pre-image is the borrow pull we already hold.
- Every remove's instance parks, never frees (§3a — see §4), so un-remove
  is re-attach, not re-instantiate.
- Every add's inverse is a park. Every move's inverse is the mirror move.
- The rack-shape pre-image is the plan's own `baseIdentity` snapshot.

Failure mid-plan (a refusal, a load failure, a timeout on one op): the Link
stops, reports which op failed, and **runs the inverse plan itself from the
journal** — it holds everything needed and it owns the rack; having the main
drive rollback over IPC adds a failure mode for nothing. The user sees one
statement: "Apply failed at <op> (<reason>) — <Link> was restored to exactly
its pre-Apply rack. Your edits are still here, uncommitted."

**The crash case** (an instantiation kills the host — §2 below): no rollback
can run in a dead process. The journal is the answer, by the deadman's own
idiom: the Link's next launch finds an uncompleted plan journal, restores the
pre-images (shape and states), deletes the journal, and publishes. The main,
seeing ack silence then the restored sidecar, reports the crash and the
restoration honestly. The one non-restorable window — a crash *during the
journal restore itself* — is the same residual the deadman accepts, and it
blacklists the plugin that caused it.

## 2. Instantiation on the Link — stage first, mutate second

**Recommendation: a two-phase plan. Phase A (STAGE) instantiates every new
plugin detached — in no rack, exactly the shape the detached-editor probe
proved — under a death mark per load. Phase B (MUTATE) reorders, removes,
attaches staged instances, seeds states.**

Slot 3 of 5 failing to instantiate therefore aborts in phase A with **zero
rack mutations** — there is nothing to roll back, because nothing was
touched. The whole Apply refuses, named per slot: blacklisted ("X is on this
machine's crash skip list — the Link cannot host it"), load failure ("could
not load right now" — the iLok-class refusal, never "not owned"), or crash
(death-marked, blacklisted on relaunch, journal-restored per §1; phase A
crashes restore trivially since the rack was never touched). Both instances
are on one machine so the plugin *exists*, but the Link's process is its own
world: its blacklist reads the same shared file (agreement by construction),
yet enforcement is at ITS load, and the spec treats the Link's answer as the
only one that counts.

Cost note: a staged-then-aborted instance parks (never free) — a failed
Apply costs the memory of its adds. Named, accepted, same §3a economics.

## 3. The class model — Create joins Commit and Withheld

Explicit classes, one per slot of the plan, extending `BorrowCommit`:

| Class | Meaning | State write? |
|---|---|---|
| **Commit** | existing slot, seeded, edited | yes — the user's edit |
| **Create** | added in the main; the Link never had it | yes — seeded from the MAIN'S instance |
| **Remove** | deleted in the main | no — instance parks with its state |
| **Move** | reordered | no |
| **LeaveWithheld** | seed never carried the Link's state | never (§5c unchanged) |
| **LeaveUnedited** | byte-equal to baseline | no |

**Create is not Withheld's problem**: Withheld's rule protects the Link's
real settings from being overwritten by defaults — a slot the Link never had
has no settings to protect, so Create seeds unconditionally from the main's
instance (captured at plan time, the same codec, the same caps). The verdict
stays a recorded fact, not a recomputed policy: Create is recorded at the
moment the user adds the slot.

**Two deliberate wrinkles, stated in the confirm rather than resolved
silently:**
- Removing a WITHHELD slot deletes settings the user never saw. Allowed —
  the rack is theirs to shape — but the ask names it ("you are removing a
  slot whose settings never made it here"), and the parked instance plus the
  journal pre-image keep it recoverable for the session.
- Moving a withheld slot is plain structure: allowed, state untouched.

The Apply confirm's asymmetry sentence grows the new counts: "adding A,
removing R, reordering, writing C, leaving W withheld and U unchanged."

## 4. Removals and the never-free rule

§3a applies on the Link exactly as it does on the main: a removed slot's
instance goes to the Link's graveyard (its `removeSlot` already does this),
alive with its state until process exit. Consequences, named: user-driven
removals add permanently to the Link's memory high-water for the session;
un-remove during rollback is re-attach of the parked node — **which requires
the Link's remove-for-plan to park re-attachably (the pool idiom from the
borrowed host: in-graph, disconnected, suspended) rather than into the
write-only graveyard**, because a graveyard node can never re-enter the
graph (the addNode fact from step 1). That is the one Link-side mechanical
change this spec forces beyond the plan applier itself.

## 5. The staleness guard moves to identity

**Recommendation: upgrade the guard, don't argue names past the hole.**

The lock argument is real — under the rack lock and lease, the Link cannot
restructure, so names-in-order *would* suffice while the lock holds. But the
guard exists precisely for the moments the lock has failed: expiry mid-plan,
a foreign supersession, a crashed-and-relaunched Link that restored a
different rack. Arguing names-sufficient means the guard is weakest exactly
when it is needed. And §3e's same-name swap stops being latent the moment
the user can legitimately reorder same-name slots in the main.

So: `baseIdentity` replaces `baseSlots` in the plan — per-index
`{name, uid, fp}` from the sidecar's identity fields, uid through the ONE
hex→decimal seam from step 3's round (`sidecarUidToStateUid` — that bug is
the cautionary tale for this exact upgrade). The Link verifies count and
per-index identity with the absent-is-no-opinion grammar (`fp` compared only
when both sides carry it, uid likewise, name always). Mismatch = the whole
plan refuses before phase A, named. The old `baseSlots` name guard stays
untouched on the per-slot lease path (out of scope, hole recorded in §3e as
before); capability-gating means no mixed-version ambiguity — an old Link
never receives a plan at all.

## 6. The Link's window, during and after

**During**: the rack-lock overlay is already up (the lock is a precondition,
and spec §4b makes it load-bearing here — doubly so with structure moving).
Add one honest line to it while a plan journal is active, driven by an
additive sidecar flag: "<owner> is restructuring this rack…". Per-op repaint
churn is already coalesced by `notifyChainModel` per mutation; the overlay
line is the statement that the flicker underneath is deliberate, not
corruption. The Link's own controls stay dead (lock) — nothing new to
refuse.

**After** (success): the overlay's restructure line drops with the journal;
on release the lock drops too and the Link's editor rebuilds from its own
model — it is the owner again, showing the new shape, with its standard
per-slot state notes for seeded slots. Parked removals are invisible, as the
graveyard always is. **After** (rollback or journal restore): identical
mechanics, old shape, plus one state note naming that an Apply was rolled
back — the Link-side user deserves the same honesty as the main-side one.

## 7. Gates (sketch, for the build step)

The plan applier's classes and ordering are pure and pinned like
`BorrowCommit`; the journal round-trips (write → restore → byte-identical
pre-images); phase-A-abort leaves a synthetic rack byte-identical; the
re-attachable park proves a remove/un-remove cycle instantiates zero new;
the identity guard's arms (absent-is-no-opinion per field, same-name swap
CAUGHT — the §3e case as a passing test at last); capability refusal for a
sidecar without `structureEditCapable`. Hands-on: kill the Link mid-plan and
verify the journal restore by content on relaunch.

## 8. Out of scope

The per-slot lease path (superseded, unchanged); in-context monitoring
(parked as ever); any change to `leakedNodeStore` semantics beyond the
re-attachable park the rollback requires (§4).
