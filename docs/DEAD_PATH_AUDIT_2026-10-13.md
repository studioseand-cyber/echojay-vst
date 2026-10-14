# Dead path audit — 13/14 Oct 2026

Sean's ask B: this week's bugs share one shape — **declared, wired, documented, never called end to end**.
Sweep the plugin for that shape, report before gating, fix only the clearly-small-and-broken class.

**Method, and its limits.** Four passes, all mechanical, then every claim checked by hand:

1. every `std::function` member (185 of them) tallied for assignments and invocations across `Source/`;
2. every reported success traced back to what it was read from — a write, a send, a queue, or a read-back;
3. every `juce::TextButton` member checked for a click handler;
4. my executor's op and read vocabulary diffed against B's `docs/CONTRACT_AGENT_TOOLS.md`.

Pass 3 is **method-limited and produces no claims below**: 22 buttons have no `.onClick`, and the first one
I checked (`compareBtn`) demonstrably works — it is painted, and the interaction is handled by the view/nav
code. A per-button check is needed before any of those 22 can be called dead. They are listed at the end as
candidates, not findings. (The same discipline as the `ls-remote` grep: a pattern written from a hypothesis
is not a check of that hypothesis.)

---

## FIX THIS ROUND — clearly a bug, small, and done

| # | Dead path | Evidence | User impact |
|---|---|---|---|
| 1 | The own rack's agent edits were handed a BARE ARRAY where `parseChainEditOps` reads `{"edit":[…]}` | `ChainHost.cpp:1913` `auto* o = v.getDynamicObject(); if (o == nullptr) return out;` | every agent `add`/`remove`/… on the open rack: "that change carried no operation this rack understands" |
| 2 | `build` was emitted as an edit op; no rack has ever had one | `ChainHost.cpp:2515` ends `unknown operation`; Sean's log 15:45:00 | `op 1 invalid: unknown operation "build"` on a Link |
| 3 | `set_wet` read `pct`; the contract sends `wet_pct` | CONTRACT_AGENT_TOOLS.md:103 | **every** agent slot-wet change refused, quoting the figure it was given |
| 4 | `bypass` read `on`; the contract sends `bypassed` | CONTRACT_AGENT_TOOLS.md:104 | **every** agent bypass refused |
| 5 | `add` dropped `wet_pct` | CONTRACT_AGENT_TOOLS.md:98 | a plugin asked for at 40% wet arrived at 100%, silently |
| 6 | A Link's rack-OUT reported success from a FILE WRITE | `PluginEditor.cpp`, my own item 10 code of 13 Oct | "that Link's OUT is now +2.0 dB" with nothing confirmed |
| 7 | The V2's rack-OUT reported the value asked for, not the value read | same sink | a clamped gain would be misreported |
| 8 | `ExecutorRead::setTarget` had **no caller in the product** | the editor even kept "the typed pointer setTarget needs" | the agent never knew which rack was open (round item 7) |
| 9 | The "Details" handler called `onLayoutNeeded()` + `repaint()`; only `resized()` rebuilds `layout_` | `EJAgentPanel.cpp` | pressing Details did nothing (round item 3) |
| 10 | The per-line plan button was labelled with its STATE and acted as the opposite | `EJAgentPanel.cpp` | a button saying "Apply" skipped the line (round item 2) |
| 11 | `drainOrphanedChatTurns` latched after its first call, and wrote only to the store | `PluginEditor.cpp` | a turn that landed after reopening was never shown (round item 5) |
| 12 | The borrowed build PUSH never said it was a build | `BorrowPendingPush::op == "build"` existed; the wire carried no `intent` | the Link armed no level match for a borrowed build (round item 4) |
| 13 | The user's own message was appended to an EDITOR-OWNED store and never synced at send | `PluginEditor.h:6355` `EchoJayWorkspace workspace { api };` | closing mid-reply lost the user's message (round item 5) |
| 14 | `makeup_cap_db` and `output_unit` existed nowhere in the plugin | B sends both on compressor blocks | the server's make-up cap was ignored (round item 8) |

## SEAN'S RULINGS ON THESE, 13 OCT 2026 (what was actually done)

1. **`onResetAuto`: FIXED.** "Double-click on pre-gain restores AUTO, fix it." Done in
   `ChainWetKnob.h`, scoped by the hook itself - only the pre-gain knob assigns `onResetAuto`, so that knob
   restores AUTO and every other bipolar trim keeps the 24 Sep behaviour of going to 0.0 dB. **This reverses
   21r item 6 of 24 Sep** ("not 'auto', which reads as 'something will decide later'"), which is recorded at
   the site so the next reader sees both rulings.
2. **The output fields: DELETED, ruling kept.** "Keep the 29 Sep ruling... I'm telling B to stop sending
   output_param / output_min_db / output_max_db / output_unit; keep makeup_cap_db." The carriers
   (`outputParams`, `outputStartDb`, `outputMinDb`, `outputMaxDb`, `outParams`, `outValue`, `outMin`,
   `outMax`) are gone, including from the serialised state. A block still naming `output_param` parses and
   says the field is **named and ignored**; `level_loop_guard` leg (21) was flipped from asserting the field
   is "carried for the make-up work that is owed" to asserting it is declined, so the ruling is enforced
   rather than documented. `makeup_cap_db` is read and honoured, bounding make-up only.
3. **The three one-ended callbacks**, and two of them turned out not to need wiring:
   - **`onRemoteEditorRequest`: DELETED as a duplicate.** Its body was identical to `onEditRequest`'s
     (`beginRemoteEditSession`), and `onEditRequest` IS wired - to the remote slot's Edit button. The
     behaviour existed; the second name is what stayed dead. Sean's intent ("remote-slot tap opens that
     plugin") is now served by a **double-click** on the slot, through that same one door. A double-click
     rather than a single tap because `selectSlot()` records that a single remote click used to auto-begin a
     session, "which coupled selection to a lease and gave the flow no visible starting point".
   - **`onSlotSubstituted`: REPLACED by a state note.** It fired on every substitution and nothing listened,
     so the notice reached no surface. The substitution now calls `addStateNote(...)` at the fire site, which
     the rack's note band already draws in both the V2's window and the Link's own - no listener for a future
     editor to forget. The hook is deleted: a notice that depends on someone remembering to subscribe is the
     fault, not the fix.
   - **`onRowDoubleClicked`: DELETED with its model's dispatch.** That model has **no view** - nothing calls
     `setModel` with it. The inline picker it belonged to was deleted on 13 Aug 2026 and replaced by
     `ChainPluginPicker`, which has its own list and **already opens a plugin on double-click**. So the
     behaviour Sean asked for is already true where the rows actually are, and wiring those two hooks would
     have been wiring a view that does not exist.

## ASK SEAN — real, but fixing it changes behaviour (ANSWERED ABOVE)

**A. `onResetAuto` — assigned twice, invoked never.**
`PluginEditor.h:3278` and `LinkEditor.h:305` both wire it (`preGainKnob.onResetAuto = …`), and
`grep -rn "onResetAuto()"` returns nothing. `ChainWetKnob`'s `mouseDoubleClick` resets the VALUE and calls
`onGestureEnd`, so a double-click on the pre-gain knob sets 0 dB but never tells the owner to go back to
AUTO. Fix is one line in the dB knob's `mouseDoubleClick`. **Behaviour change**: a double-click would
re-arm automatic pre-gain, which is what both call sites plainly intend but not what it does today.

**B. B's `output_param` / `output_min_db` / `output_max_db` / `output_start_db` are parsed, stored in the
loop's state, SERIALISED, restored — and never write anything.**
`outParams`/`outValue` have no consumer (`EJCalibLoop.h:992`, `:2460`, `:2601`). That is correct per your
29 Sep ruling — "the hold writes EchoJay's own OUT and nothing else; a plugin's own output is left exactly
where the build put it" — so this is dead *by decision*, not by accident. It matters now because item 8
asks me to honour `output_unit`, and a unit only means something if something writes that control. I have
honoured it by DISTRUST (a non-dB unit makes the figures unusable and says so). **Your call**: leave the
ruling as it is (then `output_param` stays dead and B can stop sending it), or reverse it so the hold may
write a plugin's own output control when the map names one in dB.

**C. `onRemoteEditorRequest` — assigned by the editor, invoked by nothing.**
`PluginEditor.h:3173` documents it: "Remote slot tapped: the editor asks that Link to raise its own." The
panel never calls it, so that tap does nothing today. Fixing means deciding what a remote slot's tap should
do (raise the Link's own window, or refuse with a line).

**D. `onSlotSubstituted` — fired by ChainHost, listened to by nobody.**
`ChainHost.cpp:10023` fires it on every built-in substitution; `grep "onSlotSubstituted ="` returns nothing.
A substitution is therefore silent unless some other path happens to mention it.

**E. `onRowDoubleClicked` — fired by the list, assigned by nobody.**
`PluginEditor.h:2429` dispatches it; nothing sets it. Double-clicking a sidebar row does nothing.

## DELETE

- `orphansDrained_` — the latch that excluded the only case its function existed for. Removed this round.

## Candidates needing a per-button check (pass 3, no claims)

`autoRelBtn_ autoTuneBtn_ bandButtons_ chainWarnOkBtn compareBtn contBtn_ correctBtn_ deletePresetBtn
holdBtn_ hpssBtn_ huntDynBtn_ leftBtn_ monoBtn_ pingPongBtn_ refPrevBtn resetBtn rightBtn_ savePresetBtn
settingsBtn speed1_` (+2) — no `.onClick`, but `compareBtn` and `settingsBtn` are known to work, so the
absence of an `onClick` proves nothing here. Needs the host rig (Sean's ask A) to press each one and watch
for a state change; that is a better instrument than any grep, and it is what the rig is for.

## The shape itself

Nine of the fourteen fixed items are the same sentence: **something reported a result it had not read.**
A file write reported as a landing (1, 2, 6), a value reported as set (7), a layout reported as redrawn (9),
a label reported as an action (10), a store reported as saved (11, 13), a build reported as a build without
saying so (12). The remaining five are a field or a call that simply had no other end (3, 4, 5, 8, 14).

The guard that catches this class is not a unit test of either end — both ends were fine in every case. It is
a test that goes **through** both and then reads the far side. That is the host rig.
