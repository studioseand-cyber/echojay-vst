# Overnight 23->24 Oct: the checklist, ticked as it goes

His six source items, then the chain. Updated as each one lands, so a pause costs nothing.
Build gate: single guards only until `~/Desktop/FOR_REVIEW_22oct/B_DONE_TONIGHT` exists.
If it has not appeared by 02:00: build and install anyway, mock road only, live road owed.

## 1. The dial's direction, in general
- [x] **Found the cause, and it is not the direction.** His log, two lines apart: the dial read
      "Peak Reduction" as **0.00 dB** while the card's read-back reported **33**. bx_opto exposes the
      name twice - the knob, and a read-out of what the knob is doing - and the name scan took the
      first match. The opening write then wrote the read-out's 0.0 onto the knob, so the add's 33
      became no compression. Same shape as the Vac Attack's "+0.0".
- [x] One matcher for a named control (`echojay::namedControl`), preferring a settable parameter;
      the WRITE road takes settable only. Read and write can no longer mean different parameters.
- [x] The band's low end is floored clear of zero: `target 1 dB` was becoming **0.0-1.0**, so "no
      compression" was inside the band and the Lindell finished without stepping once. Now
      `[max(0.3, t-1), t]`. **My first cut straddled the target (t-0.5 to t+0.5) and
      `level_loop_guard` 17a/17b refused it** - the band is `[t-1, t]` by a convention the system's
      prose quotes ("the band whose prose has always been 3 dB"), and nothing about his Lindell
      needed that changed. A 3 dB target is still 2-3; only the case that landed on zero moved.
      DS5c now holds the convention beside the floor.
- [x] The direction is also MEASURED on the first step: if the reduction moves away from the band,
      the declared sense is inverted once, out loud, and the loop carries on.
- [x] Guard (comp_contract_guard, GREEN): **DS5/DS5b** the band straddles the target and keeps its
      floor clear of zero; **DS6/DS6b** a dial asks no question by default and B's own `ask: true`
      still wins; **DS8/DS8b** a plugin naming the same control twice resolves to the KNOB on both
      the read and the write road, and a name matching only read-outs can be read and never written.

## 2. The dial's result on the card
- [x] **The cause:** the run registered a dial only when B's op repeated `keep_playing` - a second
      field saying what `dial_on_playback` already says. His Lindell op carried only the first, so
      the run never knew: no "Keep it playing" step was drawn, and the routing to the card found the
      plugin absent and fell through to a chat bubble. Every build-armed dial now registers.
- [x] No question after a dial: `ask` defaults to FALSE on this road (B can still turn it on).
- [x] The fell-short line is B's short shape, not "Adjusted the X's Threshold to +4.8 dB".
- [x] At target but the meters disagree says so ("is doing about 1.0 dB already; its meters
      disagree... so I left it as it is") instead of "only reaches".
- [x] The dial's step sits WITH ITS ADD, not at the end of the card (his 16:40).
- [x] Guard (agent_client_guard, GREEN 343): **KP6/KP6b** the dial's step is the row after the add
      that armed it and is Running there; KP5/KP5b still hold the landed result on that same step.
      *(My own bug on the way: I iterated `c.dialResults().getAllKeys()` - a reference into a
      temporary that dies at the end of the expression - and KP5 went red with "no dial step". Held
      in a local now, with the reason written where it happened.)*

## 3. B's two new contracts - **ARRIVED 22:18, AND DELIBERATELY NOT IN THIS BUILD**
They did not exist when this list was written; they landed with `B_DONE_TONIGHT` at 22:18, the same
minute the chain started building the five fixes below. Both are read and neither is half-built:

- **`CONTRACT_dial_step_2026-10-23.md`** - a new `op: "dial"` frame with `of`, `step_text` and a
  structured answer (`outcome` / `figure_db` / `end` / `control` / `value`, plus `levelling` for a
  limiter and `clip_drive` for a clip), run in batch order, with a 60 s playback wait and a
  `not_heard` answer. **Switched on by declaring `dial_step` in the start context's capabilities**,
  and B wrote the default to be safe: *"Without it: today's behaviour, unchanged. The released
  plugin is safe."*  So the capability is **not declared tonight** - declaring it half-built would
  have B sending dial frames this build cannot run, which is worse for him than not having it.
- **`CONTRACT_clip_drive_2026-10-23.md`** - a new dial KIND (`kind: "clip_drive"`, `control:
  "slot_in"`, `out_follows_in`, `measure: "peak_reduction_across_slot"`) that rides the existing
  `dial_on_playback` road. Real work inside the loop: a new actuator that writes the slot's IN and
  mirrors it on the OUT, and a peak-reduction measurement across the slot.
- Both are **"on b/clip-drive, not live yet"** in B's own words, so 3215 cannot send either to
  tonight's build. Nothing is exposed by waiting one round.
- **Owed with it:** an explicit refusal for a dial kind we do not implement, so a `clip_drive` block
  arriving at an older build holds the plugin instead of mis-dialling it as a drive. Quick, and it
  belongs in the same round as the kind itself.
- [x] What needed no contract, from his test-3 ruling: the dial's step sits WITH ITS ADD.

## 4. Undo all clickable
- [x] Guard (global_undo_guard, GREEN): **RA1/RA1b** three wire adds become one undo entry with a
      label that says what it takes back; **RA2** top slot first, so no remove shifts a slot a later
      remove names; **RA3** redo is refused outright rather than adding a bare copy.
- [ ] **His own retest** is what proves the rest: the Link's window opened and closed mid-run, then
      Undo all on the finished card. The rig can drive the run but not his window. *(Not mine to
      tick - it is in the report's retest steps as step 4.)*

## 5. The supersede ask replaces the refusal (his 21:06)
- [x] The ask was gated on `! isWorking()` - a WAITING run could be superseded, a WORKING one could
      only be refused, which is the 20 Oct split his 21:06 ruling removes. The question now comes
      first for both, and Start anyway stops the old run whatever it was doing.
- [x] Never repeated: the ask remembers the message it is up for, so a second Enter adds nothing.
- [x] Guard (chat_store_guard GREEN 141): SU5 a second Enter adds no second ask and repeats no line;
      SU6/SU6b/SU6c/SU6d a WORKING run is asked about, the refusal line is gone from that chat, and
      the old run keeps going until Start anyway is pressed.

## 4b. Undo on a Link (the same item, what was found)
- [x] **The cause:** his log says "undo all walked the history back to the run's checkpoint - 0
      step(s), depth now 0" - the history was EMPTY. `ChainHost::pushUndo` covers the local rack and
      a borrowed copy, a remote PARAMETER move records a `remoteValue` entry, and a remote
      STRUCTURAL edit - every agent add to a Link - recorded nothing at all. The `chain` kind cannot
      serve it: it restores through `rackFor(uid)`, and an unleased Link has no host to restore into.
- [x] New `remoteAdds` entry, recorded at the one site that sends the agent's ops, taken back as
      `remove` ops over the same wire - top slot first, each checked against the name it should
      hold, and any op whose slot has moved is dropped with a line rather than deleting what is
      there now. `Undo all` walks the same history, so it now has something to walk.
- [x] "Clickable while the closing waits" was already true (`undoAllowedNow` admits AwaitingAsk and
      AwaitingApproval); what failed was that there was nothing recorded to undo.

## 6. The live view back on
- [x] `kShadowViewEnabled = true`, with the channel-count fix that caused both crashes.
- [x] The missing audio: the slot index baked into the pre-trim node went stale when the rack
      shifted, so the tap fed nothing. It is an atomic the host refreshes on every rebuild.

## The chain  *(B's gate arrived 22:18; attempt 2 running from 23:15)*
Attempt 1 was stopped at its own red: S4 reported `level_loop_guard ***Failed` on legs 17a/17b, which
is the band convention above. Nothing was installed - the chain refuses a red - the cause was fixed,
both guards were re-run green, and the chain restarted. (It also carries a new rule of its own, from
last night's lesson: a test that TIMES OUT is re-run alone before the install is refused, because a
timeout under LTO thrash is the suite's weather and not a verdict. A real failure still stops it.)
- [x] Build (three trees, four bundles, both AAX), **S4 85/85, S5 GREEN**, installed - V2 AU
      `96420D29-5A29-3573-8148-EB7E8F6AB8BB`, V2 VST3 `0267A847-15DB-3353-BA7C-F7FB76286386`,
      Link AU `63ADDBF5-CC8F-3269-86E3-5B9F2FF93B54`, Link VST3 `EB7EDED9-4E81-3D9A-A615-EDC7DC66BBB7`
- [x] The rig: mock (156/4) and the live road against 3215 (84/7), plus a fourth mock pass (88/2) to
      confirm the row fixes. No new product red on either road.
- [x] Grouped commits: five tonight plus the docs; tree clean.
- [x] `docs/MORNING_REPORT_2026-10-24.md` written: UUIDs, every verdict, what is owed, retest steps.

## The mock road's verdict (attempt 2, 00:43-01:03) and what it found

**88 checks, 4 red, and three of them were the ROW rather than the product** - which is what a row
that finally aims at the right thing looks like:

- **I9c** - the known Link-figure-vs-param-map timing red, owed since 18 Oct.
- **B5, both legs** - the aim WORKED this time (`work_on -> ok (working on edd1155c67)`, where this
  morning it was refused twice), so the row measured a real chat at last. And it compared his message
  against **the first card on screen** while the chat held **five** - so the card "above" his message
  was an OLDER run's, sitting where it belongs. Second proxy in that row in one day: the uid this
  morning, the card tonight. It now matches the card by `cardRunKey` against the run's own
  `agentRunKey`, and says so rather than comparing when no key is available.
- **B6's new-chat leg** - "his row at -1": his message was not missing, it was **held on the
  supersede ask's chips**, waiting for an answer nobody gave. That is tonight's own ruling (21:06)
  meeting a row written for the behaviour it replaced. The row now presses "Start anyway" the way he
  would, and the press's note is printed so a run where the ask never appears is still legible.

Both row fixes are in, syntax-checked. They are harness-only; no plugin source was touched while the
live road was running.

## The confirming mock pass (03:07-03:29): 88 checks, 2 red

- **B5's two legs: PASS.** The card is now matched by `cardRunKey`, so the row compares his message
  against the card this run drew rather than against an older run's.
- **B9 found its own slot** ("the EchoJay plugin this row added is slot 3 on 162e861365") and then
  skipped loudly for a NEW reason, which is a real finding: the live view **cannot copy an EchoJay
  built-in** - "No compatible plug-in format exists for this plug-in" - because the copy goes through
  the plugin catalogue and our own devices come from the built-in registry. Owed, with the fix named
  (the built-in road, the one the guards already use).
- **B6's new-chat leg: still red, and the rig is why.** `"Start anyway" press -> unknown button
  "chip0"`: the rig's chip vocabulary answers the CLIENT's pending ask, not the editor's replace-ask,
  so it cannot answer tonight's supersede question at all. The guard holds that claim (SU5/SU6*).
  Owed: a press for an editor ask.
- **I9c**: the Link-figure vs param-map timing, owed since 18 Oct.

## Done, and committed
Five grouped commits tonight (the dial's direction; the dial as part of the build; undo on a Link with
the supersede ask and the live view; the rig's three rows; the docs). Tree clean, S4 85/85, S5 green,
installed, both rig roads run, and the morning report is `docs/MORNING_REPORT_2026-10-24.md`.

## The loop's close, 03:3x

Everything on tonight's list is ticked or named as blocked, the morning report is written, the tree is
clean and nothing of mine is running. The only unticked box left is his own retest, which is his to
do. Two items are blocked and said so with evidence: B's dial step (a capability I will not declare
half-built, by B's own design) and B's clip drive (a new dial kind, with the contracts now in hand).
Three things are owed from the small hours: the live view cannot copy an EchoJay built-in, the rig
cannot press an editor replace-ask's chips, and I9c still measures a figure that depends on the Link's
param map.
