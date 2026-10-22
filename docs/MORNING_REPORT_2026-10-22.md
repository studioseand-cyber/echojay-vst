# Morning report, 22 October 2026

Written overnight, against his list of 21 Oct ("work straight through, one heavy job at a time").
Every installed UUID, every guard and gate result, and a retest list in order. The gate's own verdict
is at the end of section 5 and is the one thing in here that was still running when the earlier
sections were written.

## Installed, in order (each one replaced the last)

| round | what | V2 | Link |
|---|---|---|---|
| 38 | item 1: the single stamp, the transcript router, seq 0 renders LAST, appVersion on /api/params/sync | `14C2EABB-BB7E-3FF4-826E-BDB228276B17` | `013AF0F9-7837-3339-9134-649E17676E00` |
| 39/40 | item 2: stage D - the row-birth gate retired, the run's row found by the run's KEY | `351725ED-5490-39B0-9EB3-610B761AD8CB` | unchanged |
| 41 | item 3: B's status frames under "Thinking...", the your-answer label on its own line when narrow | `E65853C1-EA86-302E-8E09-0BCE36618EDE` | unchanged |
| 42 | item 4: Link companion calibration loops (one hold per dialled compressor) | `0DBC553D-3622-30A4-955A-6566D890E6C3` | `A66576D8-5492-3966-A22D-20F39458BB8A` |

**The pair to test is the last row** unless the gate's own build superseded it, which section 5 says.

## 1. Single stamp and the transcript router — done

- **The single stamp.** The store appends FIRST (which creates a chat that is new, so its allocator
  has something to allocate from) and the place and id it stamps are then handed to the view's row
  and to the processor's entry. One row, one place, one identity on all three sides. Before this the
  processor's copy was stamped from a chat the store did not know yet - so: not at all - and the
  run's entry, created moments later, took a real place above it. That is his 21:22 fault.
- **seq 0 renders LAST, not first.** My stage-C comment claimed an unplaceable row was "given its
  place at the end"; his log proved that path never ran and `0` sorted FIRST, which is how his own
  message ended up under the card it had started. The comparator is now one function
  (`chatLogBefore`, EchoJayWorkspace.h) used by the editor AND by chat_log_guard, so the rule cannot
  be stated twice and drift.
- **The transcript router.** `AgentRunBridge::agentTranscript` fell back to `appendLocalResultBubble`
  when the open chat had no row for a run - which writes to the chat on screen, and the stop line of
  the run he had just left arrives exactly there. It now writes NOTHING when the run belongs
  elsewhere: the record is EJAgentHome's, it is a listener on the same client, and it has already
  written that line into the run's own chat. His 21:24.
- **appVersion on `/api/params/sync`** (`buildSyncRequestBody`), from `JucePlugin_VersionString` -
  the same source /api/me and the chat body use, written by CMake from the one project VERSION.

Guards: `chat_log_guard | chat_store_guard | agent_client_guard | bubble_parity_guard |
chains_view_guard | ui_guard` GREEN 6/6, plus `mapfps_sync_guard` GREEN (its F2 leg pins appVersion
on the parsed body, as a string).

## 2. Stage D — done

`agentRunMessage()` finds the run's row by the **run's key** - one lookup, no fallbacks. The
per-window integer was reissued by the chat replay (his 14:15, which cost a waiting card its chips,
its Stop and its Undo) and the sessionId does not exist until the server answers, which is the window
in which a row is born. The birth gate is retired; `rowBelongsInChat` is called by nothing in Source.

**The rule that replaced it, and the asymmetry matters:** refuse exactly when the RUN names a chat
and it is not this one. The run's own statement binds; the window's silence does not excuse it.

| run says | window says | drawn? | why |
|---|---|---|---|
| a chat | nothing (new chat) | NO | his 19:51: a new chat captured the card |
| nothing | a chat | YES | nothing is contradicted - an invisible live run is the worse failure |
| a chat | a different chat | NO | his 11:59 and 16:00 |
| a chat | the same chat | YES | - |

My first cut refused whenever either side was unstated and `chat_store_guard`'s INT1/INT2 went red
for the right reason: a run that states no chat would never get a row. Fixed at the cause, not by
loosening the leg.

Guards: the same six, GREEN.

## 3. The status line and the narrow label — done

- **B's status frames** (agreed as `docs/CONTRACT_status_frames_2026-10-21.md` on both sides): SSE
  event `status`, `{type, text, round?}`. The client holds the latest, B's words drawn as sent;
  **the latest replaces the previous**; it is cleared when the next step appears, when an ask is
  raised, and when the run ends. With none, the row is just "Thinking...". It rides the ENTRY's
  payload, which is where he asked it be drawn from, and is never stored as history.
- **The your-answer label** keeps its column above 240 px of card and, below that, becomes the row's
  own second line so the step text always takes the card's width.

Guards: SF1-SF10 and NW1-NW4 in `agent_client_guard`, GREEN with the rest.

## 4. Link companion loops — done

A Link had ONE `calibLoop_`, so the second dial of a build replaced the first: his two- and
three-compressor vocal chains dialled one and dropped the rest without a word. Now:

1. the tick is one worker (`tickOneCalib`) called for the primary and each companion - the body is a
   rename and a parameter, nothing else, because this is the Link's measurement path;
2. `startCalibMany` takes the whole batch at once (one at a time would have each replace the last);
3. the sidecar carries `calibExtra` beside `calib`, absent when empty, so an older V2 reads the
   primary exactly as it does today and a handover continues all of them.

`calib_link_guard` was run **before and after on the same fixture**, as he asked:
- **before:** RED - and the reason was its own prerequisite, a stale link-side archive in
  `build-release` ("rebuild it: cmake --build build-release --target EchoJayLink"). Rebuilt, re-run,
  GREEN. That is the fourth time this month a gate red has been the harness's provenance rather than
  the product, which is why the archive gets rebuilt first now.
- **after:** GREEN, with CO1-CO5 added: every dialled slot starts a hold, the first is the primary,
  each is on its own slot, `calib` still carries the primary and `calibExtra` the companions.

CO4/CO5 were red first and found a real gap: a batch that had just started was not on the sidecar
until its first window closed, so a handover in that gap would have found the primary and none of the
companions. `publishCalibState` is now called at a START as well as after a judged window.

Quick guards for the round: `chain_ack_wait_guard | chain_busy_ack_guard | calib_landing_guard |
level_loop_guard | loudness_loop_guard` GREEN 5/5.

## 5. The full gate — the first run found one of mine, in the one place quick.sh cannot look

The gate owed since 20 Oct (LTO, universal, AAX, so Pro Tools gets a current build) was started at
00:33. The first run **failed S1 at 8%**, and the cause was my own change from item 1:

```
In file included from tools/ejextract/main.cpp:42:
Source/EchoJayParamMaps.h:206:52: error: use of undeclared identifier 'JucePlugin_VersionString'
```

`appVersion` on the `/api/params/sync` body (your 21 Oct ask) reached for `JucePlugin_VersionString`,
which **only the plugin targets define**. All four bundles built clean — `EchoJay`, `EchoJayLink` and
the AAX link all completed in that same run — so every quick build and every guard passed it. The one
target that includes that header without the plugin defines is `tools/ejextract`, and nothing but the
full gate builds it. This is what the gate is for; four rounds went out on top of it last night and
none of them could have seen it.

**The fix is at the cause, not at the call site.** The header is not the plugin's alone, so it cannot
require a plugin-only macro to compile. The property is now written under
`#if defined (JucePlugin_VersionString)`, and deliberately **without a fallback string**: production
decides what to store on the version of the build that asked, so a console tool claiming the plugin's
number would be answered as though the plugin had asked. `ejextract` includes the header only for
`fingerprintForDescription` and never calls the sync builder, so it only had to compile.

Proven both directions before the gate was restarted:
- **the direction that was broken:** `ejextract` alone — red in the gate, now `Built target ejextract`;
- **the direction that must not have moved:** `mapfps_sync_guard` GREEN, where F2 pins the body's
  `appVersion` to this binary's version. That guard compiles the macro, so it is the proof the plugin
  road still sends it.

The first run was killed rather than left to finish: it had already lost S1, and I had edited a header
while its other targets were still compiling, so its verdict would have come from a half-edited tree.
Gate **b** was started at 01:01 on the finished tree.

### The gate's verdict, stage by stage

| stage | what it is | result |
|---|---|---|
| S1 | forced archive deletes, three-tree relink, every tool target | **OK** (after the `ejextract` fix above) |
| S2 | the guard binaries (70-odd, LTO) | **OK** |
| S3 | the four bundles | **OK** |
| S3b | archive provenance: the guards' link line newer than every header | **OK** |
| S4 | `ctest -L "fast\|plugins"` | **100% of 85 tests passed** |
| S5 | `link_state_guard`, two-sided and standalone | **OK** (G6/A and G7 both green) |
| S6 | the host rig, live against B's local server | **RED** — see below |

So every guard, every bundle and both link guards are green on this source. The rig is the one red,
and it is worth reading carefully, because **one cause owns five of the eight failing rows** and it is
not in the product.

### S6: one missing fact, five red rows

`host_rig` (live) failed **one** row: F3. `host_rig_mock` failed eight: F3, C11v(a), C11v(b), I1, F1,
F2 — and C5 and G1, which have been red since 19 Oct and are already yours-and-mine on the matrix.

**The cause.** Item 0b — a run surviving its window closing — has been **on by default since 20 Oct**,
when you proved it in Logic: *"card still waiting, chips live, plugin on the rack, card in the right
place."* The rig kept its own copy of that switch:

```
    plugin:   EJ_AGENT_0B=0 turns it OFF   -> on by default
    the rig:  EJ_AGENT_0B=1 turns it ON    -> off by default
```

Same env var, opposite defaults, so since 20 Oct the rig has been asserting the behaviour of a build
that does not exist. The harness's own comment was already the rule — *"the rig must set the SAME env
var the plugin reads, or it would be testing a different build"* — and reading the same var with the
inverted default is that same fault one layer down. It is the two-sources-of-truth shape again, this
time in the harness.

Then it cascaded. F3 starts a run and closes the window; with 0b on the run is still live when the row
ends, and **a live run refuses a typed message by design** ("The agent is still working on ..."). The
next five rows all type into the chat:

```
    EJAgent: typed message refused while active: "rig-classifyhold-marker: how does this kick sound"
    EJChat:  window #5 closing; it did not own the turn in flight (chat )
```

C11v(a) then reported `lastAssistant: ""`, I1 reported `row="" chips=[] altLabel=""`, F1 reported
`0 copy(ies)` of the interrupted line and F2 reported its rows at index `-1`. Five rows, each
reporting the absence as a product fault, none of them with a chat that would take a message.

**The fix, at the cause and in three parts:**
1. **the build publishes the switch** — `agent0bOn` on the editor state, from the plugin's own
   `agentRunSurvivesCloseOn()`. One authority for one fact.
2. **the harness asks the build** instead of holding a second answer, and has **no default**: a build
   that does not publish the field makes the rows say so rather than pick a side, because a guess in
   that spot is the whole of what went wrong.
3. **F3 re-aimed, with your ruling quoted** — it keeps its question (what is the user left with after
   a close mid-run?) and changes its instrument: with 0b on the run must SURVIVE, and the reopened
   window must still show it exactly once. The duplicate half, which is what F3 was always really
   about, is identical in both shapes. B1-B4 now RUN rather than skipping on a stale reason.
4. **F3 puts the bench back**, and the chat rows **set and assert their own ground** (no run in
   flight before they type). Setting it silently would hide the same thing the day Stop breaks.

### The two rig rows you asked for

**N1 — "a run started, no row drawn".** Everything else about a run's card is asserted somewhere, and
all of it presumes the card exists; the rows that read it do so after a helper has already waited for
a finished run, so a card that appeared late, or only at the end, passed every one of them. N1 is
therefore **a time bound, not a state read**: from the moment the run is active, a row must be drawn
in the chat you typed into, within 2 seconds. On the live road it passed with **the row drawn in
14 ms, rows=1**.

It also needed a product change to be an assertion at all: the editor state published no chat id, so
"the row is in the chat he typed into" compared `""` with `""`. Four of your reports this week were a
row or a message landing in the wrong chat, and no rig row could name the chat on screen. `chatId` is
published now.

**N2 — the pre-gain check, on plugins that need no licence.** The fault it is for is your 15:30 report:
a rack-level target inserted an EchoJay Limiter after FG-X 2, drove it to +12 and set the input trim to
−2.7. The row sets the pre-gain by hand, runs a build that carries `dial_on_playback target_lufs` with
a budget, and asserts the pre-gain is untouched and that no limiter was inserted. **The plugin list is
part of the row:** it builds from EchoJay's own compressor and nothing else, so no PACE/iLok plugin is
hosted or scanned by it — one licence prompt takes the whole rig down.

N2 also taught me something on its first run, and I have left the lesson in the harness. It **passed**
— pre-gain 3.00 dB → 3.00 dB, no limiter — while the rack went **from 0 slots to 0**: on the live road
the model read the rack and asked for playback, which is right, and the build never happened. "The
pre-gain is untouched" is true of a build that never ran. So the row now lives on the mock road, where
the build is scripted, and **asserts its own landing first** — one slot more than before, and it is
the compressor — before it makes any claim about the pre-gain. On the live road it skips and says why.

### The two rows that were already red: C5 and G1

Both have been red since the 19 Oct matrix and both are marked **A** on it. With the five cascade rows
cleared, they were the only two left, so I took them.

**C5 — "after Undo all the closing no longer claims the change" (your #14).** This was a real one, and
the reason it survived two "fixes" is that the rule and the sentence had different owners. `build()`
honours your ruling: once every change is undone it REPLACES the closing. The card's closing, though,
is *folded* from the opening plus the closing — and the opening is the model's own prose, which says
the thing the undo just made untrue. So the replacement cleaned one field and the fold wrote the stale
sentence straight back into it:

```
closing = "The pre gain on the Mix Bus is set to 3 dB.

            Everything this run changed has been undone, so what it said above no longer describes the rack"
```

The rule now belongs to the code that decides **what the card says** — the fold itself — keyed on
`closingBeforeUndo`, which the replacement sets and the snapshot carries, paired with the change count
so a run that lands something new afterwards folds normally again instead of latching on a field that
survived in the record.

Its only coverage was a 28-minute host run, which is why three days passed. It now has a leg in
`agent_client_guard` that takes a second, **proven both directions**: Z2u red with the fix withheld
and green with it, Z2v (a run that landed something after the undo) green either way, so neither leg
is vacuous and "never fold" could not have passed them. `agent_client_guard` GREEN 300/300.

**G1 — "the main rack strip shows the Link's real rack with no manual switch".** This one was not what
it said it was. Its detail read *"the Link's own process holds 3 slot(s); the V2's strip draws 0
(sidecar revision )"*, which reads as a mirroring fault. Two lines the row never looked at:

```
EJRig: select_link seq=1135 -> FAILED (that uid is not live)
EJRig: work_on     seq=1136 -> FAILED (that uid is not live)
```

The V2 had refused to aim at that Link at all, so the strip was never going to draw it — the row was
reporting the consequence and not the cause. Row I9c learnt exactly this on 18 Oct: *"the row's guard
tested that the STRING was non-empty, which is not the same question as whether the Link exists — and
the command that would have told it so had its ack discarded."* Same fault, four rows along.

So the Link block now resolves the uid from **the V2's own published list** (the Links this run
launched first), reads both acks, and carries a refusal into the rows as the refusal, in the V2's
words. I deliberately did **not** turn it into a blanket skip: a Link whose process is up and holding
a rack while the V2 says its uid is not live is a product fault, and the row now says which of the two
it is looking at. The strip read also had a bare 1200 ms pump where it needed a bound — your 11:32
report was a rack view that said "Reading 0 plugin(s)…" for minutes — so it waits up to 8 s and
reports how long the mirror actually took.

### What came out of the dark: B1-B4 ran for the first time

Fixing the switch had a consequence I did not anticipate — **four rows that have never run in their
lives now run**, because they were skipped for three days on a reason that had stopped being true.
Two of them are good news and two need you.

**B1 and B2 PASS.** This is the first *rig* proof of what you found in Logic on 20 Oct:

```
ok  ITEM 0b: the window was DESTROYED and the run is still there - same session, still live
ok  ITEM 0b: the reopened window ADOPTS the run's row rather than starting a second one
```

**B4 was my row's fault, not the product's.** It asserted the pre-gain reads 6.0 dB after the
reopened window presses Apply — the scripted plan's second line. Live, it read 2.00 dB, and the
model's own closing says why: *"Set the Mix Bus pre-gain to 2 dB, applied twice as asked."* It chose
2, correctly, and my row called the product wrong for it. The question is the same on both roads
(does the reopened window's Apply land on the rack?), so the scripted figure stays where it is
scripted and the live road asserts that the pre-gain **moved**.

**B3 is a question for you, and I have left it RED rather than answer it myself.** It claims:

> a run started in a window RAN TO COMPLETION with no window attached at all, and the processor holds
> its steps and its closing sentence

It got `started=1 finished=0, steps [running], closing "", parked lines 0` — the run sat there. And
the product's own log line says that is deliberate:

```
EJAgent: window #3 closed with a run LIVE - it carries on without a window (item 0b);
         a BUILD would park until one opens
```

So B3 asserts one design and the build ships another. Your 20 Oct proof was a run **waiting** on a
question, which is B1/B2 and passes; whether a run should also **finish and apply with no window at
all** is a decision, not a defect, and it is yours. Re-aiming it on my own judgement is exactly the
thing you told me not to do. **One line from you settles it:** should a windowless run apply its
build, or park until a window opens? If it parks, B3 becomes an exclusion naming that design; if it
should apply, B3 is a real red and I will chase it.

### Where the rig stands now

Three runs, each one narrowing it:

| run | live (`host_rig`) | mock (`host_rig_mock`) |
|---|---|---|
| the gate's own, 02:48 | RED 1 failed, 63 passed | RED **8 failed**, 110 passed |
| after the switch and the ground fix | RED 1 (N2's own precondition) | RED **2 failed**, 119 passed |
| on the rebuilt bundles, with F3/N1/N2 live | RED 7 (all rows fixed after it compiled) | RED **2 failed**, 130 passed |
| after G1's second fix | not rerun (owed) | **RED 1 failed, 131 passed** |

The mock road went from **eight failures to one**, and twenty-one more rows now run than did at
midnight (B1-B4 among them). The one that remains is **B3**, which is your one-line decision above.
Nothing else in the rig is red.

**C5 PASSES** on the mock road — red since 19 Oct, fixed at the fold, green in the host.

**F3, N1 and N2 all pass** on the mock road, with the build publishing its own switch. N1 passes on
the live road too (the row drawn in 51 ms). N2 skips live, by design, and says why.

**G1 PASSES** too, on its second fix: *"the Link's own process holds 1 slot(s) [Renaissance Axx (s)];
the V2's strip draws 1 [Renaissance Axx (s)] after 60 ms"*. Red since 19 Oct, and the mirror turns out
to be fast — 60 ms, where the row had been waiting out an 8-second bound for a slot that belonged to a
different Link.

G1's second fix is worth recording because the first one made it worse. Reading the ack cured the
refusal and then the uid came from the V2's own list, which no longer held a usable Kick — so the row
**aimed at the Vocal Link and went on measuring the Kick**, comparing one Link's strip with another
Link's rack and waiting out its full 8 s bound for a third slot that was never coming. The rule for
this is already written down in this harness, from row I9c: *a row sets its own target*. The target is
now picked first and the process behind it is the one every row in that block reads.

## 6. Shadow stage B — not started

Your list said "if time remains", and it did not. The night went into item 5, and I would rather tell
you that than hand you a half-wired renderer. Stage B is still parked at the clean point from
yesterday: `Source/EJShadowView.h` exists, is included by nothing and is compiled by nothing, so no
build carries any part of it. Stages C and D are untouched and still yours to review.

## 7. What is installed, and what is not

**Nothing from the gate round is installed.** The pair in `~/Library/Audio/Plug-Ins/Components` is
still round 42 from 00:18 — V2 `0DBC553D-3622-30A4-955A-6566D890E6C3`, Link
`A66576D8-5492-3966-A22D-20F39458BB8A`. That is deliberate and it is the gate's own rule: your
install condition is "S6 fully green", and S6 is not, so the tree keeps its bundles and your Logic
keeps the build you have been testing.

The gate round's work — the `ejextract` fix, `chatId` and `agent0bOn` on the rig state, and the C5
fold fix — is **built and guarded in `build-guards`, waiting on your word**. Say "install it" and it
goes on in a couple of minutes; it is a safe set (one guard fix, two published fields, one wording
fix that only changes a card whose run has been fully undone).

## What is open, in the order I would take it

1. **B3 needs one line from you** (above): should a windowless run apply its build, or park until a
   window opens? Everything else in the rig round is settled; this is the only open red whose
   answer is not mine to pick.
2. **The full gate wants one more pass** once B3 is settled, to go green end to end. S1-S5 are green
   on this source already, so it is S6 that needs the rerun, not the whole thing — and the live half
   (`host_rig`) has not run since G1's second fix, so that run is owed whatever you decide about B3.
3. **Chat-log stage E** (the model's body composed from the log) and the last writer still on the
   content fallback — the single stamp covers every writer I have found, and E retires the fallback.
4. **The undo card's claim**, your older one: it may only say "everything undone" when the rack
   actually matches the checkpoint. C5 fixed what the card SAYS after an undo; this is whether it is
   entitled to say it.

## Retest list, in order

1. **Open Logic on the installed pair and send one instruction.** A card must appear under your
   message within a blink — that is the new N1 row's subject, and it measured 51 ms here.
2. **Close the window mid-run, reopen.** The run must still be there, its card adopted once, chips
   live. B1 and B2 now prove this in the rig for the first time; your 20 Oct finding is the reason
   they can.
3. **Undo all on a finished run.** The card must read "Everything this run changed has been undone…"
   and must NOT still say "the pre gain … is set to N dB" above it. That is C5, red for three days.
4. **A build with a loudness target on a mix bus.** Your pre-gain must not move and no limiter may be
   inserted — N2's subject, from your 15:30 report.
5. **Select a Link with plugins in it.** Its rack must appear on the main strip without a manual
   switch, and within a couple of seconds. If it says "Reading 0 plugin(s)…" for longer, that is the
   G1 shape and the ack line in the log will name it.
