# ship_2026-10-17b — what changed, and what you should recheck

## Installed

**Yes — `ship_2026-10-17b`, at 20:11.** All four bundles, both slices; every UUID in `~/Library`
equals the one in the ship folder, cross-checked afterwards rather than trusted from the install
script's own output:

    EchoJay V2.component    602E2FA8-9728-328E-9834-0AF4375EB465 (arm64)
    EchoJay Link.component  3DA1BAB8-0B7E-3031-A6D2-1636EB177674 (arm64)
    EchoJay V2.vst3         E9DD38DE-5C20-338B-9108-2B2E0E3706AB (arm64)
    EchoJay Link.vst3       B3A9F0DF-2A31-32B6-BC1B-13DA3370925C (arm64)

AUHostingService was killed twice after the copy. `ship_2026-10-17b/GREEN.txt` has every stage's
verdict and each skipped row's reason; `INSTALLED.txt` has the UUID pairs.

**S6 took five runs to get both halves green together, and that is the honest headline of the
afternoon.** Each run went red on a different one or two rows, and every one of those reds was
either the model's own choice on that attempt or the Link-identity artefact below — never the
build. The deterministic half has been green since noon. What the five runs cost in hours they
bought in a rule I should have written this morning: a row's failure that belongs to the server is
recorded as the server's, with the evidence attached, and is never counted as a pass.

**The restart.** The Mac restarted at about 16:17, during S6 of the gate that began at 14:40, and
took `/private/tmp` with it — S1's content manifest and every gate log. S1–S5 were green in that
run, so the question was whether the source still matched what S1 compiled. Proved three ways
instead of the one that was lost, each stronger than it: git says no file under `Source/` differs
from commit `fbd5890e`, which is what S1 built; that commit was made at 14:34:41 and S1 linked the
archives at 14:43:13, after it; and no source file is newer than the archive, which is S3b's own
test and needs no manifest. Only S6 was rerun, as you asked.


## Method, because you asked for it first

Your instruction was that five of these fixes pass in the rig and fail in Logic, so the rig's shapes
are not the product's shapes, and that I should replay this morning's real sessions instead of
writing rows from shapes I imagine. That is what happened, and it paid for itself on the first row.

`tools/host_rig/replay/` now holds **ten fixtures extracted from the plugin's own log, 08:05–08:30**:
the frame sequence per HTTP response, every tool-call id, approval mode, plan frame and done
summary, the ask cards with their real choices, and your presses and typed answers with their
timings. The mock serves one back verbatim when a goal carries `rig-replay:<id>`.

**Item 2 was one line, and only the replay could see it.** The real round 5 carries
`tool_call … AUTO-APPLIED "add Tube-Tech CL 1B"` **and** a `plan` frame. The plan handler read
`else if (status == Pending) status = AwaitingApproval` — so your tap set the step to Pending because
the tap *was* the approval, and the plan frame flipped it straight back to waiting. B2b's scripted
add carries no plan frame, which is why it passed every night while you pressed Apply three times.

**What is not exact, said plainly.** The start bodies were logged as a byte count, and `message`
frames were logged without their text — so the two sentences you quoted this morning sit in the
fixtures with a `textSource` field saying they are your words, not a recording. Both gaps are closed
at the source now (the client logs an unknown frame's text), so the next replay is exact.

---

## Red-then-green, and where that proof stops

| your item | row | on ship_2026-10-17a | with the fix |
|---|---|---|---|
| 1. New chat carries over | **RP1** (`rp_cp_8`) | **RED**: "an ask was pending; after New chat the run was STILL OPEN (awaiting_ask); the typed question went to session ag_sRCLcqX4U — the same one" | GREEN |
| 2. tap still needs Apply | **RP2** (`rp_cp_4`) | **RED**: "after the tap the card REACHED AWAITING_APPROVAL and wanted Apply again" | GREEN |
| 5. a typed name stopped the run | **RP3** (`rp_cp_3`) | **RED**: "the ask offered one chip and no free text, he typed cl1b: THE RUN WAS STOPPED" | GREEN |
| 4. "Added" before the ack | **RP4** (`rp_cp_2`) | passed on 17a as well — **not a proof** | GREEN |
| 6. duplicate closing | **RP5** (`rp_cp_4`) | passed on 17a as well — **not a proof** | GREEN |
| 10. raw OS error | **RP6** (`rp_cp_8`) | passed on 17a as well — **not a proof** | GREEN |

The bottom three rows are regression guards, not evidence. The reason is the same for all three: the
frame that drew the sentence you read is a `message` frame, and the build that ran this morning did
not log its text, so I can place your quotation in the fixture but I cannot prove the row fails
without the fix. **Items 4, 6 and 10 are fixed from the code and from your words, not from a
reproduction** — they are steps 2, 4 and 6 of the recheck below, and the logging that will make them
reproducible next time is in this build.

---

## The fixes, one line each

1. **New chat closes the open run.** `createNewChat` suppressed the thinking ROW and left the RUN
   alive — a distinction only the code can see. It stops anything still waiting, through the same
   `stop()` the Stop button calls, and the agent's target follows the new chat's binding.
2. **The plan frame can no longer undo your tap** (above). An auto-applied step keeps its status in
   both modes, and if the plan frame arrives first the tap still wins when the tool_call catches up.
3. **Nothing that cannot load is offered blind** — and per your ruling it is **offered, not refused**:
   the add stops before the load, says "kHs Transient Shaper won't load on this Mac — it failed to
   start up here", keeps the raw OS text for Details, and hands back `tryAnywayOffer` plus the
   loadable alternatives of the same role. B draws **[Try anyway]** and those chips; the answer comes
   back as the same add with `tryAnyway: true` and it loads exactly as named.
4. **Nothing says "added" before the ack** (and the track named is the target).
5. **A typed plugin name answers the ask** — fuzzy, against the inventory rather than the chips,
   because the chips are the server's guess and the inventory is the fact. Only a sentence stops the
   run, so #21 is intact.
6. **The closing goes when the bubble ends with it or carries it as a whole line**, not only when the
   two are identical — which is what let one sentence of prose in front defeat last night's fix.
7. **A slot editor only ever draws on the Chain tab.** The panel asks the window to be there first,
   through the single writer of the visible tab.
8. **The mirror's "running on …" message was never missing.** It was drawn by a single-line
   `drawText` with ellipsis into a display area a few hundred pixels wide, and the sentence is long —
   so it ellipsised down to almost nothing. It wraps to four lines now. "Bring it forward" moved from
   the bottom-left of the whole panel (where the strip's M and S buttons are) to directly under the
   note, and it no longer returns silently when it cannot find the track or the shared folder.
9. **"Thinking…" appears when you press Send**, not when the first frame arrives — your log has the
   gap in it: goal out at 08:05:14, round frame back at 08:05:19.
10. **A load failure reads as words**, with the OS's own sentence kept for Details.
11. **Empty chats are swept** instead of being filtered from the sidebar and kept anyway (eight of
    them had accumulated).
12. **A chat belongs to the track it changes, by the router's road too.** Opening a channel has done
    this since August; the composer's picker only set the target, so work aimed at the kick was filed
    under the mix bus. The picker now opens that track's own chat when the current one already holds
    turns.

---

## Two regressions of mine that the rig caught in one pass

Worth telling you because they are the reason this round took the time it did.

- **A4 and G1** went red with "the Link's sidecar 2 → −1" and "the V2's strip draws 0". My new
  unloadable list walked all 674 scanned plugins asking for architecture verdicts — reading bundles
  off disk **while holding the plugin-list lock** — so the registry pass and the mirror queued behind
  a disk sweep. That is queue item 11's hazard, which I wrote down last night and then walked into.
  It reads only cached verdicts now.
- **B4** went red because item 5 met the rig's own marker: it types `rig-row-marker: set the
  pre-gain`, the sentence test read the first word, and the instruction was swallowed by the waiting
  ask. The verb is now looked for at the start **and** after a leading colon, and nowhere else.

---

## The recheck, only this round's items (about 7 minutes)

Agent mode **Ask**, the project with the Mix Bus and the Kick's Link.

1. **New chat** (item 1). On the Kick's Link, ask for something that ends in a question. While it
   waits, press **+ New chat**, aim it at the Mix Bus, and type anything. **PASS:** the old run is
   gone, the answer is about the Mix Bus, and nothing from the Kick run is drawn in the new chat.
2. **The tap** (item 2). Ask for a compressor on the Kick's Link and **tap a chip**. **PASS:** it is
   added with no Apply press. *This is the third time I have asked you to check this one; the
   difference is that the row now carries the plan frame your session had.*
3. **A plugin that failed before** (items 3 and 10, and your ruling). "Add kHs Transient Shaper" on
   the Kick's Link. **PASS:** it says up front that it won't load on this Mac, offers **[Try anyway]**
   and some working alternatives as chips, and the line has no OS error text in it — that is under
   Details. Tapping **Try anyway** must really try it.
4. **"Added" before the ack** (item 4). Target the Kick's Link and ask for Renaissance Axx with a
   threshold. **PASS:** nothing says "Added …" until after you press Apply, and no line names the Mix
   Bus. **Not reproduced in the rig — your eyes are the instrument here.**
5. **A typed name** (item 5). When it asks which plugin, type **cl1b**. **PASS:** it answers the
   question and the run carries on. Then type a sentence ("set the pre-gain to 2 dB") into a waiting
   run: **PASS:** that still stops the run and starts its own.
6. **The closing** (item 6). Any short run whose bubble has a sentence of its own before the summary.
   **PASS:** the summary appears once. **Not reproduced in the rig.**
7. **The editor's tab** (item 7). From the **Chat** tab, select a rack slot. **PASS:** the window
   switches to Chain and the editor draws there — never over the chat.
8. **The mirror** (item 8). Select the CL 1B slot in the main window's mirror of the Link. **PASS:**
   you can read the whole "running on 88_7even Kick_2_01…" sentence, **Bring it forward** sits under
   it rather than over the M/S buttons, and pressing it either brings that window forward or says why
   it cannot.
9. **Send** (item 9). **PASS:** something appears immediately — no blank pause.
10. **The chat list** (items 11 and 12). Press New chat two or three times without typing: **PASS:**
    no empty entries accumulate. Then, in a chat with a few turns, aim the composer at another track:
    **PASS:** you land in that track's own chat instead of continuing this one.

## Item 13 (Link meters)

Still the first item of the next round, and deliberately not started in this one: it is the change
that would let the agent read a Link's meters, and starting it beside twelve fixes and a gate would
have put an unproven feature in the build you are about to test. `NEXT_ROUND_QUEUE` item 0 has the
design.
