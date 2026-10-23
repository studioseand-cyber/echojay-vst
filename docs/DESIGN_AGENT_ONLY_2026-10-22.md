# Design: the agent is the only road in (22 October 2026)

**Status: design only. Nothing in this document is built, and nothing in the plugin behaves this way
today.** It is written because Sean asked for it on the overnight list of 22 Oct, item 5, and because
three of his faults this week were the same shape: two roads to one outcome, disagreeing.

---

## 1. What this is about

The plugin has two ways to change a rack: the **agent** (a run, a plan, a card, Undo all) and the
**hand** (the Build button, the chain panel, a slot's own controls). Everything the agent does, it does
through roads the hand also uses — which was a deliberate decision and a good one ("two build paths
that looked identical in the log is what let a whole class of failure sit undiagnosed").

But the two are not symmetrical, and the asymmetry is where this week's faults lived:

| | the hand | the agent |
|---|---|---|
| who decides the chat it writes to | the window on screen | the **run**, which may be elsewhere |
| who decides when a build applies | the press | the plan, the approval, and (since 22 Oct) nobody needs to be watching |
| what records it | a chat row, written on the spot | a card, written by the processor, mirrored by a window |
| what undoes it | the chain bar's Undo | the card's Undo all, back to the run's checkpoint |

Four faults in two days came from a road reading the window when it should have read the run: the card
above his message (test 6), the stopped card in a new chat (test 8), the card that never reached the
store (09:24), and the figure measured on one Link while another was aimed at (G1, 19 Oct).

## 2. The proposal, in one sentence

**For anything the agent can do, the agent's road is the only road, and the hand's controls become
callers of it rather than parallel implementations.**

Not "remove the buttons". The Build button stays, the chain panel stays, the slot controls stay — but
each one *starts a one-step agent run* instead of calling the rack directly.

## 3. What that buys

1. **One place decides what a change is.** Today a build has two shapes: a chain array through
   `loadChainFromJson` and replace-ops through the applier. A nameless slot is dropped by one and
   refused by the other (his 21:48). One road, one answer.
2. **One record.** Every change becomes a card with a run key, a chat and a place — the three facts
   this week's fixes had to add to rows that lacked them.
3. **One undo.** "Undo all" and the chain bar's Undo stop being two mechanisms with different reach.
4. **One place to be honest from.** "I couldn't read the plan", "that rack would not take the change",
   "the dial is still setting itself" are all sentences the run already knows how to say.

## 4. What it costs, and the two real risks

- **Latency on a knob.** A slot's own control cannot wait for a round trip. A hand-driven run must be
  able to run with no server at all: a *local run* whose plan is one step, approved by the press
  itself. If that is not built first, this design makes the plugin feel slower, which is worse than the
  faults it fixes.
- **A run for everything is a lot of runs.** The record would grow a card per knob unless a local run
  with no prose is drawn as an ordinary chat row — i.e. the card is the shape, not the obligation.

## 5. What would have to exist first (in order)

1. **The build road on the processor.** Still the piece everything else waits on: `loadChainFromJson`
   is 484 lines of editor, and the windowless road that exists today (22 Oct) is the *adds'* road, not
   that one. Until the window's road and the processor's road are one function, "one road" is a claim
   with two implementations behind it.
2. **A local run**: `EJAgentClient` able to run a plan nobody fetched, so a press is a run.
3. **A card that can be small.** One step, no prose, no chips — drawn as a row rather than a card.
4. **Undo through one door**: the chain bar's Undo becomes "undo the last run", not "undo the last
   chain op".

## 6. What I would NOT do

- I would not route **meter reads, aim or chat** through it. They are not changes, and the one thing
  this design must not do is make reading the rack cost a run.
- I would not do it in one round. Steps 1 and 2 above are each worth a night; 3 and 4 are a week, and
  the order matters more than the speed.

## 7. The open questions, which are Sean's and not mine

1. Is a knob-turn worth a card? (My own answer: no, and that is what step 3 is for.)
2. Should the Build button's run be approvable, or does the press count as the approval? (The 16 Oct
   ruling on tapped picks says the press IS the approval; this would follow it.)
3. Does a local run belong in the chat log at all? A hundred knob-turns in a session is a hundred rows.

---

*Design only. The faults it addresses are recorded in docs/REPORT_2026-10-22_pm.md and
docs/REPORT_2026-10-22_evening.md; the pieces it waits on are in docs/NEXT_ROUND_QUEUE.md.*
