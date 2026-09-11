# Go-ahead: build the rack lock. Nothing else yet.

**For the Claude Code session in `~/echojay-vst` on `macbookpro-lan`.**
Read `RACK_BORROW_REQUIREMENTS.md` §4 and §5 first — it is the decision record
this brief builds against.

The hosting survey is accepted. Answers to it are in §4 below so nothing is left
hanging, **but the survey is not the next build.** The rack lock is. It is the
only approved-and-unbuilt item, and every borrow test that follows is unreadable
without it: if you cannot tell who owns a rack, you cannot tell a refused write
from a lost one.

---

## 1. What to build

The lock from `RACK_BORROW_REQUIREMENTS.md` §4, exactly as decided:

- **UI lock only. Never an audio change.** The `Active` toggle is not touched by
  any code path in this change. If a diff of this work reaches anything that
  affects the audio graph or bypass state, the design has been misread.
- **Held by a live editor actively showing that rack** — processor renews,
  editor gates, cleared on close. Selection alone must not hold it (the
  selection is processor-held and survives editor recreation; a lock keyed to it
  strands with no visible owner).
- **Reverse contention by recency, not window state.** A local rack edit on the
  Link inside ~10s wins; the main waits and acquires when quiet.
- **Two mains: first come, first served.** The second sees the same overlay
  naming the first, and auto-acquires on release or expiry.
- **Read-only covers structure and mix writes** — add, remove, move, bypass,
  slot wet, master wet. Plugin interiors stay live: selection, scrolling,
  meters, opening a hosted editor. Do not attempt to lock parameter writes; that
  is a claim the code cannot keep.

## 2. The four amendments — all still apply

**2a. Recency needs a transport.** The sidecar has no timestamp today, so the
~10s rule has nothing to read. Add one field to `RackSidecarSlot` carrying the
last local-edit time. **It goes last**, after the other Mac's sidecar-identity
fields — `RackSidecarSlot` is positionally brace-initialised, so a field
inserted anywhere else shifts every initialiser silently and still compiles.
Assert the new field is actually being written before you trust any recency
behaviour; a field that is always zero reads as "edited at the epoch" and the
main would acquire instantly, forever, looking exactly like a pass.

**2b. A transient block must explain itself.** The recency wait is the one
refusal a user hits while doing nothing wrong. "Locked" is not an acceptable
message for it. It says the Link was edited moments ago and that this will clear
on its own — distinct wording from the FCFS overlay, which names another owner
and does not clear by waiting. Two different refusals must not share one string.

**2c. Record the `baseSlots` same-name-swap hole.** Two identically-named slots
swapped in order pass the staleness guard, because `baseSlots` is
names-in-order. **Do not fix it in this change** — write it into
`RACK_BORROW_REQUIREMENTS.md` §3e as a known latent defect with the file and
line, so the whole-rack spec inherits it as a stated problem rather than
rediscovering it as a bug.

**2d. Sibling file, `racklock-<uid>.json`.** Not a new field inside the existing
sidecar. A lock that lives inside the state file cannot be read or cleared
independently of the state, and the clearing path is the one that has to work
when everything else has gone wrong.

## 3. How I want it proven

Not "it compiles and the overlay appears."

1. **The lock is visible in the Link's own window** — greyed, overlay, named
   owner. Screenshot it.
2. **Structure writes actually refuse.** Try add, remove, move, bypass, slot
   wet, master wet on a locked rack — each one, individually. A blanket guard
   that happens to catch all six is fine; six untested assumptions are not.
3. **Interiors stay live under the lock.** Open a hosted plugin editor on a
   locked rack and move a control. If that is blocked, the guard is too wide.
4. **Editor close releases; window switch does not.** §3d: a Logic Link-window
   switch measured CTOR 0 / DTOR 0 across 20, so switching tabs must not drop
   the lock, while closing the plugin window must.
5. **Recency wait, then acquire.** Edit on the Link, immediately try to take it
   from the main: transient message, distinct from FCFS. Wait it out: acquires
   without a further click.
6. **Expiry with the owner gone.** Force quit the owning main. The Link comes
   back on its own — read this **by content**, i.e. can you actually make a
   structure edit, not "the overlay disappeared."
7. **Two mains.** Second gets the overlay naming the first, and takes it
   automatically on release.

**Every step that can silently do nothing must assert that it did something.**
A guard that returns early when it should have refused looks identical to a
guard that refused, from the outside.

## 4. Answers to the hosting survey — for the spec, not for this build

Recorded now so the survey does not go stale, and so this build does not drift
into it.

**(d) Second `ChainHost` in a constrained borrowed mode — confirmed.** The
one-graph-two-chains alternative would require tagging `slots_`, which
invalidates pinned-suite behaviour; that is a worse trade than a second
instance. The constrained mode is defined by three things you already found:
read-only on every shared file (`chain_plugins.xml`, `chain_entries.xml`,
`param_maps.json`, `session_*.json`, `chain_archive/`, blacklist appends), a
skipped constructor (no `loadFromDisk`, no `loadParamMapsFromDisk`, no
`mergeBootstrapMaps`, no `loadHelperCatalogue`, no scan thread), and — the one
that is a correctness requirement rather than a cost saving — **a borrowed
instance's `onChainChanged` must never reach `setLatencySamples`**. Host-reported
latency stays the main rack's alone. Wire that as a hard block in the borrowed
instance, not as a convention.

**(e) hazard 5 is the one to design against first.** `leakedNodeStore()` grows
monotonically across borrow/release cycles — ten racks borrowed is ten racks
held forever. That is the constraint that decides whether borrow is a
session-long mode or a per-rack operation, so the spec has to answer it before
it answers anything about the graph. Do not attempt a fix in the leak store
itself; §3a is why it exists.

**Still open, and not yours to settle in this build:** §3c (the Link's cap
tier), §5a (commit model), §5c, §5d, §5e. If the rack lock work appears to need
one of these decided, stop and say which — do not pick one.

## 5. Do not

Touch the audio path. Fix §3e. Start the second `ChainHost`. Retest the
per-slot Stage 1 guards — the per-slot flow is likely superseded and proving it
is a day spent on code that may not survive.

Report when the lock is built, gated and green, with the seven proofs above.
