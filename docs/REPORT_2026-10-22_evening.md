# 22 October 2026, evening — your 15:09–15:30 list, B's keep-playing contract, and the run

Short, as asked. Everything here is on `0BCD016B`'s successor; the UUIDs are in §2.

---

## 1. Your three faults, each at its cause

**Test 6 — the card above your message after a Link switch.** Your first hypothesis was right, and it
was a regression my 09:24 fix caused in your **21 Oct** ruling. The log:

```
15:09:19  EJAgent: START goal="build a vocal chain"
15:09:19  EJChatLog: the run's card was appended to the STORED log of chat 1792614055982 at the stamp
15:09:19  EJAgentHome: opened the run's record, key=bd319848…
```

The card reached the store **before** your message. Your message had always been stamped by
`appendLocalUserBubble` — but that is reached from the client's own "user" transcript line, which comes
back out from *inside* `start()`. That was soon enough while a card only reached the store when the run
**ended**; since this morning it is stored on the first state change inside `start()`. So the card took
the lower place, and a chat rebuilt from the store — which is what a **Link switch** does — drew it
above you. The window-reopen road hid it, because there the card is redrawn from the live row.

*Fix:* the goal is stamped **before** `start()`, which is where "the moment it's typed" actually is, and
the bridge consumes that name once so there is still exactly one copy.

**Test 8 — a superseded run's card in a brand-new chat.** A different bug, and your ruling named it.
`recordAgentRunInChat` took its target from the **client**: a superseded run is written down at the
moment the next run starts, so `runChatId()` had already moved to the chat you had just made. The old
run's "Stopped" card, eight steps and all, was filed into a chat it had never run in.

*Fix:* a row carries its own `chatId`, **stamped at creation**, and that is the only authority. Five
creation sites stamp it (a live run's row at birth, a stored card restored by the reader, an adopted
row, a held row in the merge, your own typed row). The recorder reads the row, and falls back to the
client only for rows made before the field existed — **saying so in the log**, because that fallback is
the answer that was wrong.

**Test 8 — the plan shown twice.** Two halves:
- **B's half, yours to route:** one round sends it twice — `frame tool_call … name=talk … "From what was
  measured: …"` **and** `frame unknown event="message" skipped text="From what was measured: …"`
  (`toolu_01WYwLxwvxtwQ5jVdjPGASh1`, 15:20:57). We skip that `message` frame today, so it costs nothing
  on screen — but it is one `event:` rename from becoming a third copy.
- **Ours:** the 18 Oct rule cleared the question block by **exact string equality**, and those strings
  stopped being equal — the step's label is composed when the tool_call arrives, while the client then
  *prepends* the talk's text to the question. Same sentence, two spellings.

*Fix:* normalised prose instead of exact equality, and when the block says **more** than the label, the
block's text becomes the row's. Nothing lost, nothing twice, inside the step as you asked.

## 1b. B's keep-playing contract (`CONTRACT_keep_playing_2026-10-22.md`)

`keep_playing` is parsed where `dial_on_playback` is. The processor owns the list of plugins a run must
keep the playback for — filled as the dials are read, cleared **once per run** in the one predicate both
run-minting roads consult. The client asks before it tells you to stop; while a dial is unfinished it
holds the prompt and names the plugin: *"Keep it playing while the bx_opto Pedal sets itself — I'll say
when it's done."* The listen's timeout is untouched, so this can extend a listen but never make it
endless. Two facts from two owners: the list from the processor, "has it landed" from each slot's own
`dialPending` (the open rack's `SlotInfo`, and a Link's own sidecar).

**Not done, and named rather than implied:** the contract's point 2 — *"show the dial as its own step on
the run card, with the plugin's name"* — is not implemented. The dial is announced in the playback line
and in the log, not as a numbered step of its own. That is a card-layout change and I did not start it
without you; the contract's points 1 and 3 (never say "you can stop" while a dial is pending; the usual
prompt once they are all done) are what the legs below assert.

---

## 2. Verdicts

### The suite and the install

| | |
|---|---|
| **S4** `ctest -L "fast\|plugins"` | **100% of 85** |
| **S5** `link_state_guard` | **GREEN**, both sides |
| `agent_client_guard` | **GREEN, 306** — KP1–KP3 (B's contract), Z4c–Z4e (the plan once), Z2u/Z2v |
| `chat_store_guard` | **GREEN, 105** — OR1–OR3 (test 6's order), PL1/PL2 (test 8's placement) |

Built from **deleted archives across three trees** (`LoudnessLoop.h`, `PluginEditor.h` and
`EJAgentTools.h` all changed), then 72 guard targets, four bundles **and both AAX**.

| installed | UUID |
|---|---|
| **V2 AU** | `5D9E8688-6067-33F5-AC5E-B7F3C19F86BD` |
| **V2 VST3** | `49C670C2-7530-3F8B-ADB8-3E5798E97751` |
| **Link AU** | `C903B5B9-F667-3222-99BE-B1D564894E92` |
| **Link VST3** | `E6C87BB4-A35D-3134-B0F7-0DF48BB18689` |

The Link changed this time (it did not this morning): `LoudnessLoop.h` is in its compile set, so B's
`keep_playing` parsing is in both plugins.

`~/Desktop/FOR_REVIEW_22oct/A_RIG_DONE` was written at **18:12:10** and touched again at **20:29:54**
when you stopped the live run. B was never waiting on me after 18:12.

### The rig — the mock road

**Best run: 2 failed, 150 passed, 11 skipped.**

| row | verdict |
|---|---|
| **B5** a Link switch mid-run leaves your message above the card — live, and again after the run finished | **ok** (both halves) |
| **B6** a superseded run's card does not follow you into a new chat | **ok** |
| **B6 (2)** …and your message is first in the new chat | **FAIL — owed, see §5** |
| **I9c** the Link's mirrored card carries the plugin's figure | **FAIL — owed since this morning** |

An earlier run in the same chain read 3 failed / 143 passed; two of those three were **my** B6 handing
on a live run (`EJAgent: typed message refused while active`), which took `13a` and `A4b` down with it.
Both rows now stop anything still going and assert the bench is quiet — and they have been **moved to
the end of the suite**, because a new row must not be able to reorder an established one.

### The rig — the live road (3215, 65c7c51)

Preconditions checked before it started, as you asked: `/local/health` **ok**, the tree it names
(`/Users/SeanD/echojay-saas`) at **65c7c51** with nothing uncommitted, and a **round served**.

**Stopped at your word after 83 minutes: 12 scenario rows (7 pass, 2 fail, 3 skip), 43 checks ok,
9 failed.** `B2-live`, `B3-live` and `B4-live` are among the passes — the live versions of answering a
waiting run by chip and by typing.

Of the nine, seven are the live-model family (it chooses its own plan and pace) or the collateral
above. **Two are worth your eye and are in §5:** `B3d` (Undo all after a windowless build did **not**
restore the rack live: `slots 0 -> 4 -> 4`, while it does on the scripted road) and the two Link-figure
rows `A4-live`/`I9c`.

**One live run earlier in the chain was VOID and it was my fault:** I edited `host_rig.mm` while the
chain was about to compile it, so the rig refused with "the rig did not compile". That is the repo's own
"no source edits during a build" rule, broken by me, on the file the running chain needed.

---

## 3. The commits

Today's work is in two commits on top of this morning's eight (the whole tree was committed
this afternoon, so there is nothing else outstanding but the docs commit this report rides in):

| commit | what it covers |
|---|---|
| `de93eb31` | the product: the goal stamped before `start()`, a row's own `chatId` at five creation sites, the question block cleared by normalised prose, and B's keep-playing contract |
| `ee66418b` | the rows: OR1–OR3, PL1/PL2, Z4c–Z4e, KP1–KP3, B5 and B6 — and B5/B6 moved to the end of the suite, with the reason in the message |

Nothing is left dirty except this report and the rig's own drawings, which go in with it.

---

## 4. Exact retest steps

**Test 6 (the order, Link switch) — two minutes.** Two Links, each with its own chat. In one
Link's chat, send `build a vocal chain`, press Go, switch to the other Link while it works, switch back.
*Expect:* your message first, the card under it — both while it runs and after it finishes.

**Test 8a (a superseded run) — two minutes.** Leave a run waiting on its chips. Make a **new** chat and
send something else. *Expect:* the new chat holds your new message and the new run only. The old run's
"Stopped" card stays in the chat it ran in — go back and check it is there.

**Test 8b (the plan once) — one minute.** Any run that proposes a plan. *Expect:* the plan once, inside
the step that carries Go / Change / Cancel. Nothing repeated below the card.

**Test 9 (B's keep-playing) — three minutes, needs B's build serving `keep_playing`.** Ask for a chain
whose compressor dials on playback. Play. *Expect:* when the listen has had its minimum, the card does
**not** say "Got it, you can stop" while a dial is still working — it says *"Keep it playing while the
<plugin> sets itself — I'll say when it's done"* — and the usual prompt appears once every dial has
landed. The dial is **not** shown as its own numbered step (§1b).

**If anything is off:** `~/Library/EchoJay/logs/echojay-0.log`, and the lines worth grepping are
`EJChatLog:` (a card reaching the stored log at the stamp), `EJAgentRow:` (adoption, the by-key update,
and the "this row carries no chat of its own" fallback), `EJLoudness:` (which plugins carry
`keep_playing`) and `EJAgent: playback wait` (the listen and why it is still going).

---

## 5. Blocked or skipped

Each of these is owed, not quietly dropped:

1. **B6's second half** — "in the new chat his own message is the first thing in it" reads
   `his row at -1, first card at -1`: the window published **no rows at all** at that moment, so the
   claim cannot be made (and its first half would have passed against an empty list, which is why the
   second check exists). Two candidates, both unproven: the publish came from the other instance's
   editor, or the rows were gone by the time of the read. The claim itself is covered from two other
   directions meanwhile — OR1/OR2 in the guard and B5 on the Link-switch road.
2. **B5 and B6 in their final position** (the end of the suite) have not been run there. Their verdicts
   above are from the position they were written in.
3. **`B3d` live** — Undo all after a windowless build did not restore the rack (`0 -> 4 -> 4`) while it
   does on the scripted road. Possibly a real product red on your own ask; it needs the live road again
   to say more, and I will not guess at it from one run.
4. **The Link-figure family** — `I9c` (skipped last night, failing today: "no slot carried both facts")
   and `A4-live` ("the Link's landed field still has no Threshold fifteen seconds after the run"). Both
   measure a figure that depends on the Link's param map; in the afternoon's run that map was served at
   13:08:09, around or after their attempts. The likely cure is the rows setting their own ground.
5. **`A4b` and `13a`** (the chat-turn rows) — collateral from my B6; the cause is removed but their
   green is owed to the next mock run. `13a` also has a blind spot worth one line: it has a loud-skip
   branch for a spent chat pool but none for "a run is in flight", which is why it reported a refusal as
   a failure.
6. **`C1-live`** — 4 of 5 attempts held; one ended with no closing and no opening. Unattributed.
7. **B's contract, point 2** — the dial as its own numbered step on the card (§1b).
8. **The live road after scenario row 12** — not run; you stopped it, and the rows beyond it are
   unexercised today.
