# Next round — the queue, in order

Rewritten 10 Oct 2026, after the round that closed items 1–4. Nothing here is started.

## 1. THE REMOTE-CONTROL REMOVALS (plan §6) — the deferred half of stage 5

Stages 1–5 are in. Stage 5's **removals** are not, and the reason is a coupling worth
reading before anyone picks this up.

The plan's migration note says the V2's saved borrowed **copy** must be ignored rather than
applied, or the rack doubles. That is only true once the borrowed host is **gone**. While it
is still the edit-staging path — and it is: **48 uses in `PluginProcessor.cpp`, 45 in
`PluginEditor.cpp`**, with builds still landing in `borrowHostIfActiveFor` — that copy is
**load-bearing**. The 4 Oct ruling exists because of it: a chain built on a Link mid-lease
and saved without switching racks lived *only* there, and dropping it is how Sean lost one.

So the deletion and the migration move together, and together they are a re-routing of
~93 call sites so **every edit becomes a command**. That is this item, and it is a round.

What goes when it does (plan §6): `rackLeaseEngage`/`Release` and `rackLeasePrior_`,
`setAttachBypassed`/`attachBypassed_`/`setLeaseBypass`, the `intendedBypassed` overlay dance,
the rack arm of `leaseGate_`, `leaseBaseRev_`, `rackLeaseMuteWant_`, `rackLeaseEditPending_`,
`slotWetParkedSaid_`, and the borrowed host entirely. Six bugs retire with it.

The behavioural seams are already named and already false (`Source/EJRemoteControl.h`), so the
deletion is hygiene rather than behaviour — which is exactly why it must not be rushed at the
end of a long day. I have swallowed 241 lines twice in this repo by walking braces.

## 2. The ring's measured latency

`docs/LINK_REMOTE_CONTROL_PLAN.md` §10 lists this as not established, and it still is not:
the ≤1 buffer figure is from the structure of the code, not a stopwatch. The contract asks for
the Link to log the wall-clock delta between a frame's write stamp and the block that applied
it, asserted at both buffer sizes. The drain is wired; the measurement is not.

## 3. `tools/mapfps_test` is not in the gate

It is registered nowhere in `tools/tests/CMakeLists.txt` and currently reports **19 failing
assertions**, almost all text pins over source that has since moved. The suite's own rule is
that every test is in the gate or carries a written exclusion like `comp_render_check`'s —
right now it is neither, which is the one state that rule forbids. Gate it and fix the 19, or
write the exclusion. **Sean's call.**

## 4. Flagged, needing someone else's decision

- **Sean:** the merged tree has **two** stores for reference sets — the kept preset feature and
  integration's new reference library.
- **Sean:** `level_slot_guard`'s summary separator moved with the integration merge
  (`gain_db 2.5`, not `gain_db=2.5`). Ruled 10 Oct: keep the re-aim, no product change. Noted
  here only so the change is not rediscovered as a regression.
- **B:** emit `option: "commercial"` rather than collapsing it to `pushed`;
  `docs/ECHOJAY_API_CONTRACT.md` does not state the current chat-stream refusal shape.
- **B:** `set_level` is now the **rack record**, not a Level slot. B's `CONTRACT_AGENT_TOOLS.md`
  should say so if it still describes a Level slot.

## Done this round, for the record

1. **The GR model re-derived** — all seven known-stale legs green as real passes;
   `kKnownStaleLegs` is empty. A unit bug (a `Plain`-weighted slot tally subtracted from a
   `K`-weighted chain tally, in the figure shown to the user), a tap nothing reset at the
   window, and a ruler rig our own ruling had given a limiter.
2. **The data fix** — the four empty arrays gone, `pinnedProjects` forwarded, `baseUpdatedAt`
   sent when the read supplied one. New gated `userdata_write_guard`.
3. **Remote control stages 1–5** — the audio detour and the lease mute gone (Sean's acceptance
   test green on rendered audio); the four value ops acked; the value ring with RT appliers for
   the four figures and `set_param` on the message thread (contract corrected); `open_editor`
   with embed proven by pid; undo under remote control, one step per gesture.
4. **Agent mode** — `ExecutorDo` on top of A2's read half, the Settings switch (a real one now:
   the dev-mode gate is gone), `echoJayOnly` and `can_act` in the start context.
