# Morning report, 26 October 2026

## Installed

**The full-tier build is installed** — S4 read 100% of 85 and S5 was green both sides, which is the
condition you set. LTO, universal, from `build-guards` (the gate's own bundles), AU cache cleared and
`AUHostingService` killed so you cannot get the old Link back.

| | |
|---|---|
| **V2 AU** | `5ECD2CDF-B05C-3474-BB61-B89FB4A852AC` |
| V2 VST3 | `BB17B0D8-2AA6-34CA-82D8-E9EFD689254F` |
| **Link AU** | `14C9DC21-E827-30CA-A5C6-903A5101B356` |
| Link VST3 | `2B67A837-B358-36C8-A127-31AEA50D34D5` |

Commit `86d4e5d5`, not pushed. The gate was brought forward to 23:59 rather than 03:00, because
everything on the list that was going to land had landed and the gate's own S1 does the forced
archive delete that a plain build does not.

**And one thing to know before you test anything in Pro Tools:** Pro Tools loads the **AAX** build,
which is **2.26.4, built 29 September** — four weeks behind. `tools/install_local.sh` writes
Components and VST3 only; it has never touched the AAX path. Every install this month went to Logic.
That is also why yesterday's log looked like it had two writers in it (item 9 below).

## The gate

S1 (forced delete + relink, three trees), S2 (the guard suite, LTO, universal), S3 (the four
bundles), S3b (archive provenance), **S4 100% of 85**, **S5 GREEN both sides**. The rig's mock pass
passed; its **live** pass came back RED on **two** rows, and neither is tonight's work:

* **H2** — `THE CARD SAID NOTHING, there is no claim to judge, so this row proves nothing and fails`.
  The live model produced no card for that prompt.
* **H3** — the build answered *"I have no fresh reading from that track yet — play the loudest
  section and I will measure it."* That is the `needs_playback` refusal working correctly: the rig
  has no transport running, so there is no reading to quote.

Nothing red was caused by last night's changes, so nothing was fixed-at-the-cause overnight on their
account. 22 rows skipped, all the usual live-road ones.

## What is done

1. **The live view's smoothness — both halves, and the bench proves it.**
2. **The refusal count** — a standing refusal now says how many moves it swallowed.
3. **The disappearing message** — a hole found and closed, **and the rig then disproved my own
   hypothesis** (see below). Still open as a fault.
4. **The EchoJay Limiter's ceiling** — cause found exactly, fixed, guarded.
5. **The dial's bounds** — B's contract implemented from his branch.
6. **The reduction/threshold runtime check** — your approval of yesterday, built.
9. **Which build Pro Tools loads, and the missing `[pid]`** — answered, nothing changed.

## Item 3: what the rig settled, and what is still open

I instrumented the rig to name which window published each snapshot, and it answered:

> his message WAS drawn in the new chat and was gone again by the snapshot these checks read
> (onScreen rows now: 0, user rows 0, cards 0). The window that drew it published as editor
> 5392236544; the snapshot these checks read came from editor **5392236544 — the SAME window**

So it is **not** two editors taking turns at one state file, which was my leading theory. The row
genuinely left that window's list, and **no clear-site line appears in the log** — and every
`chatMessages.clear()` has carried one since 11 August. Something empties that list down a road that
does not log. The only other mutation in the file is the assignment at `PluginEditor.cpp:20448`,
guarded by `if (added > 0 || stampedLate > 0)`, which cannot produce zero rows — so the next step is
to instrument that assignment and re-run the B6 row alone. That is the first thing I will do when you
say go.

The fix I did ship for item 3 (a dropped plain row now gets the home a dropped card has had since
22 October) stands on its own merits and is guarded, but **it is not this fault.** I would rather tell
you that than let a green guard imply a cure.

## What is not done, and why

**7 (dialling a slot already on the rack) and 8 (clip_drive).** Both are new agent operations and
both stand on **`dial_step`**, which is not built — it has been queued behind your faults since the
25 Oct round. Item 7's own contract says so: *"Everything else is the dial step contract,
unchanged."* I would not start a new agent op at 22:45 with the gate due at 03:00: a half-landed op
makes the model send frames the build answers wrongly, and the only honest proof of either is a rig
run. Order when you say go: `dial_step`, then item 7, then item 8. Item 5 (the bounds) is a
dependency of both and is already finished, so the dial will clamp each control to its own range the
day it lands.

---

# Your tests, click by click

## A. The live view drag — do this one first

This is the fix you most wanted, and the one I can least prove without your hands.

1. Open Logic. Put a **non-PACE** plugin on a Link's rack (Apple's AUDelay or any EchoJay device is
   ideal for a first look; the Vac Attack is fine too).
2. Open the V2 on that rack and open the **live view** on that slot.
3. **No agent run.** If the strip says an agent is working on the rack, that is a regression and I
   want to know.
4. Take one knob and drag it **slowly, continuously, in one direction** over about three seconds.
   - **PASS:** it tracks your hand. No jumping back, no stepping, no fighting.
   - **FAIL:** it stutters or snaps backwards while you hold it.
5. Now drag **fast**, back and forth, for a few seconds. Same question.
6. **Let go** and watch for a second. The knob should stay where you left it, then stay put — the
   Link is the authority again 250 ms after you release, so if the plugin itself moves it (an
   envelope follower, say) you will see that, which is correct.
7. **The wobble:** with the live view open and your hands off, watch the plugin's window and the
   slot meters for ten seconds. **PASS:** completely still. **FAIL:** any periodic shimmer, twitch
   or re-draw — that would mean something else still writes on the 100 ms tick.

What changed, so you know what you are looking at: while your hand is on a knob (and for 250 ms
after), FOLLOW no longer writes **that one parameter** into the copy — every other knob still
follows the Link on the same frame. And the clip container's seven layer properties are now only
written when they are actually wrong, instead of ten times a second.

## B. The refusal count

1. Start an agent run on a rack, and while it is working try to turn a knob in the live view. It
   refuses, as before, with the reason.
2. Turn it **ten more times**. The log now says: `EJControl: 10 moves on the live view have now been
   refused for the same reason — …`
3. Stop the run. The log says: `that refusal is over — N move(s) … were refused while it stood`.
4. **PASS:** both lines appear and your knobs work again after the stop.

## C. The limiter's ceiling (B's fault of 25 Oct)

1. Ask the agent for a mix bus that ends in **two** limiters, with the EchoJay Limiter first.
2. When the build finishes, open the EchoJay Limiter in slot 4 (or wherever it landed) and read its
   **CEILING**.
   - **PASS:** it is the figure B asked for (**−1 dB** in his case), not −0.1.
   - And the **last** limiter in the chain is still at **−0.1 dBTP** — that ruling has not moved.
3. The log says it out loud: `the chain has settled, so slot N … takes ceiling −1.00 dBTP — it is NO
   LONGER the last slot`.

## D. The reduction/threshold check (your approval)

1. On the **Vac Attack**, ask the agent to set its **Reduction** to a value (not "3 dB of gain
   reduction" — that is item 7 and is not built).
2. With audio playing, let it apply.
   - **If the map really is wrong**, the card now says so instead of a confident success: *"…it read
     back, but the slot is doing the same work as before (x dB in-to-out before, y dB after). A
     control called "Left Reduction" that changes no gain reduction means either this plugin's map
     names the wrong knob, or that is not the control for what was asked."*
   - **If the Reduction knob really does raise reduction** (which is what you now think), the check
     stays quiet and the card reads as before. Either answer is useful: it settles the map question
     from measurement rather than from either of our opinions.
3. With **nothing playing**, repeat: the card must say **nothing** about the map. A plugin is never
   accused without a measurement.

## E. The chat (item 3)

1. With a run waiting in one chat, press **New chat** and type a message.
2. **PASS:** your message stays on screen. Go back to the first chat: its run card is still there.
3. The log now says `a user row was written to the STORED log of its OWN chat …` if a switch ever
   has to move one.
