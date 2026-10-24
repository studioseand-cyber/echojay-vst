# Overnight, 24 -> 25 Oct 2026 — checklist

Rules in force tonight: Logic and Pro Tools closed. **No PACE/iLok plugins** in the rig or on the
bench (the CL 1B exception needs Sean at the Mac). One heavy job at a time; B is working too, so
every build/gate/rig/bench run takes `~/Desktop/FOR_REVIEW_22oct/HEAVY.lock` first.

| # | item | state |
|---|---|---|
| 1 | the persistence fix (View only back on; withheld = !seeded + born-here; 4 guards; bench on a non-PACE plugin; commits) | code + guards DONE, build queued behind B's lock |
| 2 | empty rack — diagnosis only, from Logic's launch | **DONE** — answer below, goes in the report |
| 3 | readback fix (b): "pending" republishes promptly, with the guard | code + SH19 DONE, in the same build |
| 4 | the "meters disagree" rule patch held from earlier | **DONE** — applied clean from the held patch |
| 5 | design notes only: layer 3, and slot meters | **DONE** — docs/LIVE_VIEW_LAYER3_2026-10-25.md, docs/SLOT_METERS_2026-10-25.md |
| 6 | 03:00 at the latest: full gate (LTO) + rig on everything since ce254b33, no PACE | not started |
| 7 | install the full-tier build if S4 and S5 green (control ON only if all four guards AND the bench proof are green) | not started |
| 8 | docs/MORNING_REPORT_2026-10-25.md, then touch A_NIGHT_DONE_25 | not started |

## Log

- **22:1x** Item 1 begun before the overnight note arrived. Done so far: `kLiveViewControlBuilt`
  back to **false** (his ruling: ship View only until the persistence road is safe);
  `BorrowCommit::classify` gains `seededFromLink`/`bornHere` and a fourth verdict
  `LeaveUnseeded`; `borrowSlotVerdicts` passes the recorded seeding fact and the record's
  `bornHere`; `BorrowSlotRecord::bornHere` added and set at all three creation sites (the picker
  add, the deselect re-base, and the post-ack re-base — the last one ONLY when a created identity
  exists, because that arm's own comment said "created here (or unknown)" and the unknown case is
  the hole the settings fell through).

- **22:38** B holds `HEAVY.lock` (taken 22:33, "hook commit of b/chain-shapes"). Retried at 22:38
  and 22:41; a waiter script now takes it the moment it frees and runs a **NO-INSTALL** validation
  build (the install comes from the full tier, item 7). Non-heavy work done in the meantime.
- **22:40 item 3** the dial's state is now its own publish trigger: `dialStateFingerprint` (pending
  AND confirmed, per slot) in `EJRackSidecarFill.h`, compared in `publishRackSidecar` and allowed to
  bypass the settle. The read-back's look budget was extracted to
  `echojay::agent::readBackLookBudget` so SH19 can hold it: with no pending reported the confirm
  road has a budget of **one look**, which is why his 21:32 figure was never waited for at all.
- **22:41 item 4** the held `calib_disagree_next_round.patch` applied clean (`EJCalibLoop.h`):
  "disagree" now means opposite sides of the band or more than `kSensorMarginDb` apart, and two
  readings both short means STEP. Both runaways the old gate was built for are opposite-sides cases
  and stay held.
- **22:45 item 5** both design notes written, each with its audio-thread cost (layer 3: **nothing**
  on the audio thread, 0.7-0.9 ms per second on the Link's message thread, measured; slot meters:
  **nothing new** — the tallies are already taken there for the sidecar) and its fail-safe list.
  Both notes end with the same recommendation: **slot meters before layer 3**, because layer 3
  insures against a fault no plugin in his rack has been shown to have.

## Item 2 — the empty rack, answered

**The Link restored its rack from the session, and the session had no rack in it.** The 0 was a real
empty rack, published 18 seconds after the restore — not an early publish racing a restore.

```
22:08:18  EJScan: cache loaded, 2426 entr(ies)                       <- the new process starts
22:08:25  EJLinkState: setState uid=4814a16004 (foreign/seed) chain=0 slot(s) in the chunk;
                       host has 0 slot(s) now                        <- THE ANSWER
22:08:25  EJLinkState: registry snapshot (state restored) uid=4814a16004 name="Aitch_4_01"
22:08:43  EJBorrow: fresh sidecar read for 4814a16004 -> valid, 0 slot(s)
22:08:49  EJBorrow: the picker's add on 4814a16004 carries a base of 0 slot(s) FROM THE LINK'S
                    SIDECAR - the mirror says 1, and the mirror is not the authority
22:08:49  EJLink: applied add count=0->1 model=1 lease=1 seq=1792876129 applied=1/1
```

`chain=0 slot(s) in the chunk` is Logic handing the Link its saved state with no chain in it. The
Link's save path **does** write the rack (`obj->setProperty("chain", chainModelToVar())` in
`LinkProcessor::getStateInformation`), so a chunk with no chain means **the chunk was saved before
the CL 1B existed on that Link** — i.e. the project was not saved between the add at 21:30 and the
restart at 22:08. Restarting Logic to pick up a new build does not save the project.

`(foreign/seed)` in that line is a **labelling artefact, not a finding**: `ownChunk` requires the
registry index, and the registration lines come immediately AFTER the setState line, so at the
moment it was printed the index was -1. The chunk's uid was this Link's own. Worth tidying so the
label cannot mislead a future reading; it changed nothing here.

**What the log cannot tell us:** whether he saved. `LinkProcessor::getStateInformation` logs
nothing, so a save leaves no trace at all. One line there — the chunk's slot count at save time —
would make this question answerable in future instead of inferred. **Proposed, not built** (item 2
is diagnosis only).

## Later log

- **22:48** B is holding one continuous lock (folder made 22:33, owner rewritten 22:44 for a second
  job), so the heavy queue has not opened yet. My waiter retries every 2 minutes and will take it
  the moment it frees; the stale rule (2 h + nothing running) does not apply yet. Everything that
  does not need the lock has been done in the meantime.
- **22:49 item 1, hardened twice after re-reading my own change:**
  - the unseeded verdict is handed on as `{withheld = true, edited = FALSE}`. With no baseline,
    "edited" means "differs from nothing", which is true of a copy nobody has touched - passing it
    on would have told him "1 slot changed, not written" about a slot he never touched. The reason
    goes in the log instead. SH18j holds the count.
  - SH18h/SH18i put the same slot through `StructureEdit::computePlan`, not only through
    `classify`: no Commit op reaches the Link for a slot with no claim to be newer, and the same
    slot WITH a claim does produce its Commit - so the guard cannot pass by refusing everything.
  - Checked the whole road rather than the gate alone: `computePlan`'s own line
    `if (c.withheld) { ++p.withheldEdited; continue; }` is what makes the refusal real, and it was
    already there.
- **22:49 the commit split is prepared** (`scratchpad/split.py`): control / persist / readback /
  calib / guards, partitioned by what each hunk says, with path-first routing. One compromise,
  named: tonight's guard legs (SH13, SH17, SH18, SH19) are one contiguous region of
  `chat_store_guard/harness.cpp` and cannot be split by hunk, so they ride in ONE commit whose
  message says which legs belong to which fix. The CONTROL round and the persistence fix are
  separate commits, as he asked.
- **22:49 the bench leg for tonight is written** (`persist`, in `tools/shadow_live_repro`): it asks
  the one thing a pure-function guard cannot - whether applying an older state over a newer change
  really destroys it on a live hosted plugin. Subject is **Apple's AUDelay**, the one Apple-made
  effect in this machine's scanned list, so **no licence and no supervision**. `run_legs.sh` now
  takes `EJ_REPRO_PLUGIN_NAME` / `_ID` and defaults to the CL 1B, so every supervised leg reads
  exactly as before.
- **22:52 the fourth guard is one that already exists, and that is better than a new leg.**
  "An agent build on a Link rack lands with its values" is `link_live_edit_guard`: two real
  processes, a real V2 and a real Link, which builds a slot onto the Link's rack **through the
  shipped path** (`chainArrayToReplaceOps` -> `editOpsToWireVar` -> `borrowPushBuiltChain`) and then
  changes that slot's gain with the rack still addressed. Its own header says why it has to be two
  processes: *"A state-only leg would have passed on the broken build: the V2's mirror was always
  correct."* It is labelled `fast`, so it runs inside S4 tonight.

  That makes it the real test of whether the born-here exception works end to end - if my
  persistence change broke the picker/agent road, **that guard goes red**, and it would be my
  change. A new unit leg could not have said that.

  So the four guards are: SH18c (a control change survives close and reopen), SH18/SH18a/SH18h (an
  unseeded copy never overwrites the Link, with the old rule in the leg as the known-bad), SH18d
  plus SH18i (a picker add then a change still lands, at the gate AND through the plan), and
  `link_live_edit_guard` (a build on a Link rack lands with its values, in two processes).
- **22:52** The publish stamp for the dial fingerprint moved into the stamp block beside
  `lastPublishedRackRev_`, after the settle gate - this function's own rule is that the stamps are
  taken in the one place that knows the publish is really going out, and I had put mine next to the
  field it describes instead.
- **22:53 queued, in order, all three waiting on the heavy lock:** the validation build
  (NO_INSTALL) + `chat_store_guard` + the non-PACE bench leg; then the commits once that is green;
  then the full gate at 01:45 at the latest (its own wrapper takes the lock, and the gate refuses
  to install unless S4 and S5 are green and no host is open).
- **22:56-23:01 item 1, the validation build: product GREEN, guard harness RED, now fixed.**
  `build rc=0` - all four bundles compiled clean, so every product change tonight builds. The guard
  harness failed with nine errors, all one mistake: `using SE = LinkShm::StructureEdit;` where
  `StructureEdit` is a **namespace**, not a struct, so it needs a namespace alias. Fixed and
  recompiled clean.
- **23:02 A RULE I BROKE, recorded rather than quietly fixed:** that recompile ran while B held the
  lock. My command chained `cmake --build` after the `mkdir` attempt with `&&`/`||` instead of
  gating on it, so the lock attempt failed and the build ran anyway - about a minute of one-target
  compile. Nothing of B's appears to have suffered, but the rule is the rule and the mistake was
  mine. Everything heavy from here is queued inside a script that cannot run without the lock.
- **23:01 item 1, the bench proof: GREEN on a non-PACE plugin.** The `persist` leg on Apple's
  AUDelay, 523 blocks rendered under four state calls, all four assertions passing:
  the parameter moved; state B really differs from A (so the plugin serialises the change, without
  which the rule would be guarding nothing); **applying the OLDER state put the parameter back -
  the change was DESTROYED**, which is exactly what the borrow commit did to his Threshold at
  22:09:05; and re-applying the newer state brought it back, so the loss was the stale WRITE and
  not a reset. The premise the whole rule rests on is now proven on a live hosted plugin. The CL 1B
  proof still waits for him in the morning.
- **00:06 a trap in MY OWN stale rule, caught before it bit.** B holds one lock folder and rewrites
  `owner` for each new job (five so far: wizard-identity, wizard-validators, security-env,
  matrix-caps...). On this filesystem the FOLDER's mtime stays at its creation time (23:02), so my
  2-hour check would have declared the lock stale at 01:02 and removed it **while B was actively
  working**. Two fixes, and the second is the one that matters:
  1. the age is now taken from the newest thing in the lock, `owner` included;
  2. and the instruction's SECOND condition is implemented, which my first version simply did not
     have: "over 2 hours old **and its owner isn't running anything**". If anything heavy is
     running, the lock is left alone and the script says so.
  The queue is still waiting, which is the correct state - B has had a legitimately busy hour.
