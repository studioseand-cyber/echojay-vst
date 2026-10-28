# B6: what this morning's pass actually says, and the instrument it needs (28 Oct 2026)

B6 is the last open product item on remaining-work §2 — *"the disappearing new-chat message (A: partly
fixed, real cause still open)"*. Round 35b's pass skipped it again, and this time the skip's own text
plus the log narrow it to two candidates. Written down because the next pass can settle it with three
log lines, and because the row's name ("the disappearing message") may be the wrong name for it.

## What the row does, and what the log shows

The leg (`host_rig.mm`, B6): Ask mode → type `rig-rowask-marker…` → **wait up to 40 s for the run to
be waiting** (`agentState` is `awaiting_ask` or `awaiting_approval`) → **new chat** → type
`rig-auto-marker…` in it → wait up to 15 s for the supersede ask's chips (`askChipsUp > 0`) → press
"Start anyway" → assert the order of his row and the old card.

The log, in order:

| time | line |
|---|---|
| 11:34:26.712 | `filed chat 1793187266712 …` and `the window's drawn rows were cleared by starting a new chat - 20 row(s) went` |
| 11:34:26.712 | `chatHistory cleared by starting a new chat - 20 row(s) went` |
| 11:34:26.717 | `a run's line belongs to chat 1793187004645 and the chat on screen is 1793187266712 - the editor writes NOTHING here` |
| 11:34:27.641 | `the run's card was appended to the STORED log of chat 1793187266712 at the stamp, with no window involved` |
| ~11:34:42 | the row skips: *"his message WAS drawn in the new chat and was gone again by the snapshot … onScreen rows now: 0, user rows 0, cards 0 … the SAME window … a disappearing-row fault"* |

**The run WAS waiting** (`awaiting_approval` is in the log at 11:34:43, when the row stopped it), the
chat ids **differ**, and the rig's `sendChat` goes through **`sendChatMessage`** — which is the
function the supersede gate lives in. So all four of the gate's conditions looked satisfiable:

```cpp
const auto runsChat = agentClient() != nullptr ? agentClient()->runChatId().trim() : juce::String();
if (auto* ag = agentClient(); ag != nullptr && ag->isActive()
    && runsChat.isNotEmpty() && currentChatId.isNotEmpty() && runsChat != currentChatId)
```

and yet **`askChipsUp` was 0 after an explicit `report_state`** — the ask was never up.

## SETTLED, 16:15: neither candidate. The row and the product disagree about which gesture is under test

The instrument answered on its first pass, and not in the way either candidate expected. It fired
five times, every one of them *"no ask needed: the run is in THIS chat"* — and **not once for B6's own
typed message**, because it only logs while a run is live and by then there was none. The log says why,
in the same millisecond as the new chat:

```
16:00:59.034  EJChat: filed chat 1793203259034 ... (the new chat)
16:00:59.035  EJChat: the window's drawn rows were cleared by starting a new chat - 20 row(s) went
16:00:59.035  EJStores: chatHistory cleared by starting a new chat - 20 row(s) went
16:00:59.035  EJAgent: STOP requested in state awaiting_ask round=1 unfinished=1
16:00:59.035  EJAgent: state awaiting_ask -> stopped
```

**Pressing New chat stops the live run**, deliberately, and the code says so in Sean's own words:

> *"A new chat is a new conversation. Anything still waiting belongs to the one being left, so it is
> STOPPED here, through the same stop() the Stop button calls … (Sean's item 1, 17 Oct)"*

So the supersede ask **cannot** fire for the first message in a new chat: the run it would offer to
stop was already stopped by the press that made the chat. B6 waits 15 s for an ask that the product
has no reason to raise, then skips. **It is a row defect**, and it has been masking its own subject
for days: *"a superseded run's card does not follow him into a new chat"* needs no ask at all.

### The row fix

Expect **no** ask after a new chat, and say why; assert the order, which is what the row is about.
`askChipsUp == 0` there is the product behaving correctly, not a missing surface.

### The one real question left, which is Sean's

Two of his rulings meet on this road and the gestures are different, so both can hold — but only one
of them is implemented:

| ruling | gesture | what happens today |
|---|---|---|
| **17 Oct, item 1** — *"a new chat never inherits the last one's run"* | **New chat pressed** | the run is stopped. Implemented. |
| **23 Oct, 21:06** — *"Before a new run supersedes a live one, the plugin asks 'This will stop the run on &lt;Link or channel&gt;. Start anyway?' … **Never cancel a run silently**"* | **a message typed** while a run is live elsewhere | the ask is raised. Implemented. |

**New chat stops a live run with nothing said to the person.** There is a log line; a log line is not
telling him. His 21:06 sentence is absolute ("never cancel a run silently"), and the 17 Oct one says
nothing about whether the stop is announced — so the two are compatible either way, and which way is
his call:

1. **New chat says so afterwards** — the new chat's first line reads "the run on &lt;X&gt; was stopped
   when you started this chat". Cheapest, keeps New chat instant, and satisfies "never silently".
2. **New chat asks first**, like the typed-message road: "This will stop the run on &lt;X&gt;. New
   chat anyway?" Consistent with 21:06's shape, but it puts a question in front of a gesture that is
   currently instant.
3. **Leave it** — the stop is implied by "a new chat is a new conversation", and the log is enough.

I have not chosen. Option 1 is what I would build if asked.

## The two candidates, as they stood before the instrument answered

1. **The supersede ask did not fire although a run was waiting in another chat.** Then the typed
   message superseded that run **silently**, which is precisely what Sean's 21:06 ruling forbids
   (*"Never cancel a run silently"*). The card appended to the **new** chat's stored log one second
   after the message supports this: that is a run starting in the new chat.
2. **The window never rendered what the store received.** The 11:34:27.641 line says the card went to
   the store *"with no window involved"*, and no `rendered chat … from the stored log` line follows,
   and **no `clearChatMessages` site fired after 11:34:26.712** — so the window's list was not
   emptied by any clear. That is consistent with the rows never reaching the window, rather than
   being removed from it. (The window held 2 rows by 11:34:45, when the next new chat cleared them,
   so rows did arrive eventually — after the snapshot.)

Those are different faults with different fixes, and the row's name fits only the second.

## The instrument, which is three lines

The same technique that cracked B9 this morning (where three instruments in a row were each wrong
about the cause until the last one):

1. **At the gate, log its inputs** whenever a message is typed while any run is active: `isActive`,
   `runChatId()`, `currentChatId`, and which branch was taken. A gate that does not fire is invisible
   today; it should say so and say why.
2. **At every REPLACEMENT of the window's rows**, not only at `clearChatMessages`: the one site that
   assigns `chatMessages = rows` already logs its render, but a path that leaves the list untouched
   while the store grows logs nothing at all. The store-side append should say whether a window
   render followed it.
3. **In the published state, the window's row count beside the store's**, so the rig can tell "the
   window never had it" from "the window had it and lost it" without inferring either.

`dropProvisional` is **not** the cause, and that is worth recording because its own comment nominates
it: *"A drop that takes the user's message instead of the bubble presents as 'my message vanished'"*.
It removes **one** assistant row by id, `findProvisionalIdx` returns -1 for id 0 (every user row), and
the row here went to **zero** rows. Its two `jassert`s are compiled out of the Release archive the rig
links, though, so the guard it describes does not exist in the build that matters — a Release-safe
refusal there is cheap insurance and is included with the instrument.

## Why it is not in this commit

Rounds 36 and 37 are the install gate for `19070a63`, and they measure that archive. A source change
now would invalidate them, which is the whole reason the install has a gate. The instrument lands with
A1a, in the round after the install.
