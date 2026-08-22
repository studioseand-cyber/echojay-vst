# Whole-rack borrow — implementation spec

Written 21 Aug 2026 against `RACK_BORROW_REQUIREMENTS.md` (the decision
record) and the hosting survey (accepted; answers in `RACK_LOCK_BUILD_BRIEF.md`
§4). **A spec, not a build.** Every §-reference below is to the requirements
doc unless said otherwise. Recommendations on the five open items are stated
as recommendations with reasoning — none is treated as decided.

---

## 0. The one-line shape

The main plugin gains a SECOND `ChainHost` in a constrained borrowed mode
(confirmed, brief §4d). Borrowing a Link's rack: acquire the rack lock, pull
the rack (slots + state) over the existing sidecar/codec machinery, build it
in the borrowed host, run it on that Link's ring audio in the existing solo
seam, edit locally, commit back explicitly, release. The Link bypasses its
whole rack once and streams dry for the duration.

## 1. leakedNodeStore() growth — answered first, because the mode falls out of it

**The constraint (§3a):** plugin instances are never freed. Naive per-rack
borrow instantiates the borrowed rack fresh each borrow and parks it forever
on release — N borrow/release cycles of the SAME rack cost N racks of
permanent memory. That shape is unshippable for a mixing workflow whose whole
point is hopping between racks.

**The answer: borrow is a per-rack OPERATION to the user, backed by a
session-long instance POOL underneath.**

- One borrowed `ChainHost` exists per main-plugin instance, created lazily on
  first borrow, living until process exit (never destroyed — same §3a logic
  that makes destroying hosts dangerous makes a session-long member the safe
  shape).
- Borrowed instances are parked, on release, in a **reusable pool keyed by
  plugin identity** (format + uid/fp, the same identity `stateFitsPlugin`
  matches on) instead of the write-only `leakedNodeStore()`. The next borrow
  checks the pool before instantiating. The pool never frees anything — §3a's
  rule is untouched; what changes is that parked instances are *findable and
  reusable* rather than only leaked.
- Growth bound: **distinct plugin identities ever borrowed**, not borrow
  cycles, not distinct racks. Ten sessions of hopping between two racks that
  share five plugins costs five instances, forever, which is the minimum any
  design can achieve under §3a.
- **Reuse is proven by content, not assumed** (amendment, 21 Aug 2026): the
  whole memory argument rests on a parked instance being as good as a fresh
  one, so `borrowhost_test` pins it — a REUSED instance seeded with a state
  blob must be parameter-identical to a FRESH instance seeded with the same
  blob (same parameter count, same values, same state read-back). A plugin
  that fails this check **falls back to fresh instantiation**: its identity
  is marked pool-ineligible for the session (the failed instance parks
  unreusable, the old leaked-store fate), every later borrow of it
  instantiates fresh, and the log names it — so a stateful-across-reuse
  plugin degrades to today's per-cycle cost for ITSELF only, loudly, instead
  of silently poisoning a borrowed rack with residue from a previous borrow.
- `leakedNodeStore()` itself is not modified (brief §4e forbids it); it keeps
  covering the main host's removals. The pool is the borrowed host's own
  parking lot with the same never-free rule.
- The user-visible consequence, stated honestly in the spec because it must be
  stated honestly in the product: memory high-water = the union of plugins
  across every rack borrowed this session. A borrow that would instantiate new
  identities is the moment to surface cost, if we ever do.

**Consequence for the mode question:** borrow/release is a lightweight
per-rack operation (the UX §1 asks for — select, edit, deselect), and nothing
about it is session-scoped except the pool and the host, which the user never
sees.

## 2. The borrowed ChainHost — constrained mode, per the confirmed survey

Constructed with a `Borrowed` mode flag that changes exactly three things
(brief §4d):

1. **Skipped constructor**: no `loadFromDisk`, no `loadParamMapsFromDisk`, no
   `mergeBootstrapMaps`, no `loadHelperCatalogue`, no scan thread. Plugin
   resolution uses the PRIMARY host's lists via existing accessors
   (`resolveByName`, the scanner rows) — read-only sharing.
2. **Read-only on every shared file**: `chain_plugins.xml`,
   `chain_entries.xml`, `param_maps.json`, `session_*.json`,
   `chain_archive/`, blacklist appends. `saveToDisk` and every persist path
   are no-ops in borrowed mode, enforced in the methods, not by caller
   convention. Death marks are the one deliberate exception: they are
   process-scoped and mutex-shared by design (survey b), and a borrowed
   plugin crashing the host is exactly what they exist to record.
   KNOWN UNGUARDED STATIC WRITER (recorded at step 1): `markPopoutOnly`
   writes the machine-wide `popout_only.txt` flag and is `static`, so it
   cannot carry the per-instance mode guard. Acceptable because it is not on
   this list, no borrowed-mode code path calls it, and `borrowhost_test`'s
   byte-identical gate covers its file regardless — if a borrowed path ever
   grows a call to it, the gate fails before the convention is trusted.
   DEATH-MARK ATTRIBUTION (verified at step 1): a borrowed plugin's mark is
   `phase \t path \t name` — no rack identity — and the primary's
   consumption blacklists by plugin path with "crashed the host during
   <phase> (deadman)"; every user-facing surface (the load refusal, the
   withheld panel) names the PLUGIN and the phase, never a rack. Nothing
   points at the wrong rack — the attribution is plugin-scoped and machine-
   wide by design, so no fix; at most a future nicety is a "state restore
   (borrowed)" phase string, additive.
3. **The latency hard block** — the correctness requirement, not a cost
   saving: a borrowed instance's `onChainChanged` must NEVER reach
   `setLatencySamples`. Wired as a hard block: in borrowed mode the host does
   not accept an `onChainChanged` that touches host latency (the main
   constructs it with a callback that only refreshes borrow UI), and a debug
   assertion in `ChainHost` refuses latency mirroring when the mode flag is
   set. Host-reported latency stays the main rack's alone (survey c): the
   borrowed chain's latency lives on a side stream the host cannot see; in
   solo it is monitoring latency and acceptable; in-context compensation
   stays parked (§8).

## 3. Audio path — the existing solo seam, one substitution

Where `editInst_->processBlock(editBuf_, noMidi)` runs today (the ring-steal
in `processBlock`, PluginProcessor.cpp ~898) the borrowed host's
`process(editBuf_, noMidi)` runs instead. The solo crossfade overwrite at the
end of `processBlock` is unchanged. Specifics:

- **No try-and-skip for the whole chain** (survey e1). The borrowed host is
  called unconditionally for its ring slot, like the main's own
  `chainHost.process(buffer)`. The real synchronization question is not a
  swappable pointer — it is **a chain being built and torn down on the
  message thread inside a host whose graph the audio thread is concurrently
  processing**: borrow-engage loads N plugins one by one, release tears the
  rack down, both while `processBlock` keeps running. That exact concurrency
  is what the MAIN host already survives daily — `loadPluginAsync` →
  `rebuildGraph` → re-prepare during playback (the live-add path, the AI
  build's progressive loads, recall's clear-then-restore), all riding
  `AudioProcessorGraph`'s internal prepare/process synchronization. The
  borrowed host is the same class using the same graph machinery under the
  same threading contract, so it inherits the same guarantee rather than
  needing a tryEnter of its own. Because this claim is load-bearing, it gets
  its own hands-on proof: **engage a borrow DURING playback** (and release
  during playback) with audio running through both paths — no glitch beyond
  the designed crossfade, no assertion, no dropout on the main's own chain.
- **CPU is named, not hidden** (survey e2): both chains run in one
  `processBlock` on the main track's core. The spec accepts this with one
  mitigation: the borrowed host processes ONLY while a borrow is engaged and
  soloed; there is no idle cost.
- Ring cushion/re-seek behavior is inherited unchanged from Stage 1.
- **Parked-node cost, measured at step 1**: prepareToPlay on the borrowed
  graph, empty pool 0.008 ms vs ten parked 0.022 ms — render-sequence
  rebuild cost grows linearly with pool size by construction (the sequence
  includes every node; suspension is checked at RENDER time,
  juce_AudioProcessorGraph.cpp:887) and is trivial at that slope. THE
  CAVEAT THE PROBES CANNOT MEASURE: graph PREPARE is not gated by
  suspension, so a sample-rate change calls every parked REAL plugin's own
  `prepareToPlay` — a pool full of samplers re-allocating could stall where
  ten trivial probes did not. Step 2's hands-on battery includes a
  sample-rate change with a real-plugin pool; the named mitigation if it
  stalls is a pool flush on sample-rate change (parked instances demote to
  the graveyard — reuse lost for them, stall avoided, never-free preserved).

## 4. Transfer, matching, and the lock

- **Pull**: slots from the rack sidecar (names, order, bypass, wet, identity
  fp/uid/version — all already published); per-slot state over the existing
  ctrl-cmd pull, N times, through `LinkShm::stateToB64/FromB64` (one codec,
  §6). Cap questions are §3c/§5e below.
- **Matching**: `stateFitsPlugin` per slot, apply-time re-check included —
  unchanged policy, one author. Outcomes feed §5c's surface.
- **The rack lock is a precondition**: borrow begins by holding the lock
  (built, proven). While a borrow is engaged, **the borrow pins its own uid**
  — the lock declaration and the borrowed target come from the borrow state,
  not from `effectiveChannelUid()`. This deliberately closes §3f's soft-
  selection hole for the borrow's duration: a chat activation may move the
  rack VIEW, but it cannot move a live borrow or its lock.
- **The Link side goes dry by the lease's machinery, rack-scoped**: the
  engage/restore/heartbeat/expiry/dead-id state machine is reused with a
  rack-scoped claim (a `scope:"rack"` field, additive). An old Link that
  cannot honor rack scope never engages it — capability is feature-detected
  from the sidecar (a `borrowCapable` flag, additive at v:1), and a main
  never offers borrow against a Link that does not announce it. Restore on
  expiry restores ALL slot bypasses from saved state, same one-restore-path
  rule the slot lease keeps today.

## 4b. Why Apply is safe: the rack lock is load-bearing, not a courtesy

Recorded at step 3 (22 Aug 2026), so nobody later removes the lock thinking
it is only UI politeness: **the rack lock is what makes rack-scale Apply
safe.** Every commit payload rides `baseSlots` (names-in-order), verified by
the Link at apply time — and per-slot Stage 1 lived with the staleness
window where the Link's user could reorder between capture and apply. Under
a whole-rack edit the lock refuses the Link's local structure writes, and
the rack lease refuses remote `editOps`, so the rack the main captured
`baseSlots` from **cannot change shape** for the lease's duration. The
same-name-swap hole (§3e) cannot bite here either — nothing can swap. Remove
the lock and Apply inherits every per-slot staleness hazard at rack scale.

## 5. The five open items — recommendations, each with its reasoning

**§5a commit model — recommend explicit Apply & Release, with continuous
local keep as crash insurance.** Push-back-on-deselect is an implicit commit;
a mis-click or crash loses a session's edits with no moment the user said
"write this" — the requirements doc already names this the most dangerous open
item. Stage 1's *Apply and Release* was explicit for exactly this reason, and
its `editEnd(keepState)` precedent gives the other half: capture the borrowed
rack's state continuously (the state-cache machinery the borrowed host
already carries), so a crash or lease death loses nothing locally and a
reopen can re-seed. Deselect without Apply asks — through `presentReplaceAsk`,
the one confirm implementation — "apply these edits to <Link>, or discard?"
Nothing silent in either direction.

**Lease death mid-borrow — recovery offer, never unstated loss** (amendment,
21 Aug 2026): if the rack-scoped lease expires under a live borrow (the Link
restored itself — its process relaunched, renewals lapsed, or a foreign claim
superseded), the borrow disengages audio immediately (the Link owns the
sound, same rule as the slot lease), but the uncommitted edits are CAPTURED
first — the `editEnd(keepState)` precedent, rack-wide: every slot's current
state is kept locally. The UI then says exactly what happened and what is
held: "<Link> took its rack back — your edits are kept. Re-borrow to
continue from them, Apply to send them now, or Discard." Apply from kept
state runs the normal per-slot commit filtering (§5c) against the Link's
CURRENT rack — if the rack changed shape while control was lost, the
mismatching slots refuse by identity, named, rather than writing into the
wrong plugin. The one thing that never happens is the edits evaporating with
only a released overlay to show for it.

**§3c the Link transfer tier — recommend a THIRD named pair, set today to the
session tier's values.** The pull is a local JSON file between two processes:
no HTTP body limit, no Postgres row — request-class caps (256 KB/slot) are
protecting a thing that is not present, and they already refuse states the
session tier was raised for (the 1.1 MB sampler). The header's instruction —
never align the two existing tiers — is honored by giving this third consumer
its OWN constants (`kLinkTransferMaxSlotBytes/Total`), initialized to the
session values (4 MB / 16 MB: it is a document-class transfer, the user's own
machine, bounded by what a session may carry) and free to diverge later.
Anything that leaves the machine keeps the API tier untouched. CONFIRMED BY
CONTENT (ChainHost.h): `kSessionStateMaxTotalBytes` is the 16 MB
**whole-chain budget across all slots**, sitting beside the 4 MB per-slot
cap — same Slot/Total shape as the API tier. So §5e's refusal arithmetic is
per-slot 4 MB and rack-total 16 MB: a 16-slot rack of ordinary states fits
with room; only a rack genuinely carrying more than a session itself could
save is refused.

**§5c withheld slots at rack scale — recommend loud, up front, and
commit-guarded.** A banner on the borrowed rack the moment the build settles:
"N of M plugins came across without their settings — this is NOT <Link>'s
sound", with the withheld slots marked in place; not a scrollable notes list.
And the hard rule that matters more than the display: **Apply never writes a
withheld slot's state back.** A slot that arrived at defaults must not commit
defaults over the Link's real settings — per-slot commit filtering by the
same match verdicts that withheld the pull. The asymmetry (edit 3 slots,
commit 3, leave 2 untouched) is stated in the Apply confirm.

**§5d the Link's window while borrowed — recommend greyed with the overlay,
not blank, not a live mirror.** The lock overlay already exists and is
proven; borrowing changes its wording ("Borrowed by <owner> — its edits
return when it releases"), nothing structural. A live read-only mirror means
streaming the main's uncommitted edit state back to the Link continuously — a
second sync channel with drift risk, buildable later if wanted, not needed
for correctness. Blank alarms ("where did my rack go"). Greyed-with-named-
owner keeps the §4 principle: a lock the user can explain.

**§5e total-bytes overage — recommend refuse, with a named list; no partial
borrow.** §2's own argument decides this: a rack borrowed minus one slot "is
mixing something that does not exist" — the entire reason whole-rack replaced
per-slot. A partial borrow quietly reintroduces the falsified chain, at rack
scale, behind a success-shaped UI. So: any slot whose state cannot come
across within the §3c tier, or a total over the total cap, refuses the borrow
before anything engages — naming the slots and sizes, so the user can act
(shrink the state, or accept editing on the Link). Note the asymmetry with
§5c is deliberate: identity-mismatch withholding (§5c) is survivable-with-
banner because the plugin still loads and the user is told; a size refusal
happens before the borrow, so nothing half-engages. If hands-on use shows
refuse-by-default too blunt, the escape hatch is an explicit "borrow without
slot N (bypassed here AND on the Link)" choice — honest because a bypassed
slot sounds bypassed on both sides — but that is an extension, not the
default.

## 6. Gates, in the house shape

- `borrowhost_test` (script-built like `racklock_test`): borrowed-mode
  ChainHost writes NOTHING — run a build/borrow/release cycle in a sandboxed
  HOME (the `state_match_test` disk-guard idiom) and assert every shared file
  is byte-identical after; assert the pool reuses by identity (second borrow
  of the same rack instantiates zero new) AND by content (§1: reused-and-
  seeded parameter-identical to fresh-and-seeded, with the pool-ineligible
  fallback arm exercised by a deliberately failing stub device); assert the
  latency block (mode flag set → latency mirror refused).
- Codec and identity matching: already pinned (`ringseek_test`,
  `state_match_test`); NOT re-proven per §6 of the requirements — the shared
  machinery is the proof surface.
- Lease-gate rack scope: extend `ringseek_test`'s lease section with the
  scope field's engage/refuse arms.
- Hands-on proofs mirror the rack lock's seven, plus: the §5c banner on a
  deliberately mismatched slot, Apply's per-slot commit filtering verified by
  reading the Link's state after release, and the §5a discard arm.

## 7. Build order

1. Borrowed-mode `ChainHost` + pool + latency hard block + `borrowhost_test`
   (no UI, no audio wiring — gate-provable alone).
2. Rack-scoped lease engage/dry-stream on the Link (capability-flagged),
   solo path wired through the borrowed host — the solo path first, per
   requirements §7.4, because it already works per-slot and proves the
   transfer at rack scale.
3. Apply & Release with per-slot commit filtering (§5c/§5a together — they
   share the verdict plumbing).
4. §5d wording, §5e refusal surface, hands-on battery.

## 8. Explicitly out of scope, unchanged from the requirements

In-context monitoring and delay compensation (parked on the drift-spec
question); fixing §3e or §3f (recorded, inherited as stated problems — §4 of
this spec *pins around* §3f for the borrow's duration but does not fix it);
any change to `leakedNodeStore()` itself.
