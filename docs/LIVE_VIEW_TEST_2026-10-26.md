# Click-by-click: the live view after "the copy leads"
### Build: V2 AU `4F3640F0`, Link AU `A5A3A9A9` (quick tier, arm64, no LTO). His four faults of 26 Oct, each with the thing to watch.

**What changed, in one line each:** AUDelay no longer crashes Logic when its slot card is clicked;
the copy owns its knobs while its view is open, so a drag moves on screen and in the sound with
nothing writing back into it; nothing on a timer touches the plugin's view once it has settled; and
the message thread's tick is now measured rather than guessed at.

---

## Test 1 — AUDelay (the crash, and the timer that stops)

1. Open Logic on the project with the Link racks.
2. Put **AUDelay** on a Link's rack (the V2's `+` picker, or the Link's own window — either).
3. In the V2, aim at that Link.
4. **Click the AUDelay slot card once.**
   * **Expect:** the live view opens and shows AUDelay's own panel. **No crash.** Before this build
     this is where Logic went (Apple's own view asserted 24 ms after we moved it between parents).
5. **Take your hands off the mouse and watch the panel for ten seconds.**
   * **Expect:** nothing moves at all. No shimmer, no wobble, no re-drawn border.
   * In the log, about two seconds after it opened: `EJPane(main): the maintenance timer has STOPPED`.
     That line is the fix for the wobble: after it, nothing on a clock touches that view.
6. **Drag AUDelay's Delay Time knob slowly, over about two seconds.**
   * **Expect:** the knob follows your hand the whole way, continuously, and you hear the delay move
     with it. No pull-back, no jump, no freeze.
7. **Let go.**
   * **Expect:** the knob stays exactly where you left it. (If the plugin itself quantises the value,
     it may settle onto its own nearest step about a second later — once, never while you hold it.)
8. **Now open that Link's own window on its track and turn the same knob there.**
   * **Expect:** the V2's view follows within about a second. That is the half of the mirror that is
     still alive: a change that did not come from you.

## Test 2 — CL 1B

Steps 3–7 of Test 1, on the CL 1B slot card.

* **Expect** the same: the knob follows the hand on screen and in the sound, and stays put after you
  let go. This is the one you reported as "I hear the change, but the knob doesn't move".
* While you drag, the card must not show "That didn't reach the Link".

## Test 3 — Vac Attack (the map is NOT changed)

1. Open the Vac Attack slot card on a Link's rack.
2. **Turn its REDUCTION/threshold knob by hand**, then **its GAIN knob by hand**, a few seconds apart.
3. Tell me the two times. The log prints the parameter name for each move, and that settles whether
   the plugin's names match its panel — the question `docs/VAC_ATTACK_TWO_KNOB_TEST.md` was written
   for. **Nothing about the map has been changed**, by your ruling.

## Test 4 — replacing a plugin in a slot (his 11:20 faults, build A3C54E19)

1. On a Link's rack, put **AUDelay** on a slot and click its card: the live view opens on it.
2. **Delete it** from that Link's rack and **add another plugin in its place** (the CL 1B, or anything).
3. **Click the new plugin's card.**
   * **Expect:** the panel is the NEW plugin's. Before this build the deleted plugin's own panel was
     adopted into the new one's container - Apple Delay's sliders under a Tube-Tech CL 1B's title.
   * The log now names what it put there: `NativeClip2: putting <class> (this editor's OWN view) ...`.
     If it ever says **ADOPTED ON A GUESS**, that road is a VST3 editor's and worth telling me about.
4. **Then turn one of its knobs.**
   * **Expect:** it moves, and the sound follows. No bar saying "this view is not following the plugin
     on the Link".
   * In the log, when a slot's plugin is replaced under a live view, the Link now says so:
     `EJShadow: slot N's parameter list has CHANGED under the tap (digest X -> Y ...)`.

## And the one thing I would like you to say either way

**Does the live view feel like using the plugin?** That is fault 4, and the figure behind it is now
measured: the V2's own 20 Hz tick, worst and mean, with a live view open —
in the rig, with a live view open on AUDelay, the worst tick was **2.8 ms** and the mean **1.97 ms** against a 50 ms period, so on that evidence the thread is not where the heaviness is. If it still feels heavy, that number is where I start.

---

### If something is wrong
Say which test and which step. The log line to quote, if you can, is the one nearest in time to what
you saw; everything in this round writes one.

### To go back
`bash tools/install_local.sh build-guards` (the last full-gate LTO bundles), with Logic closed.

---

## What the real-host test proved, and what it could not

Run with `EJ_RIG_FAST_LV=1` on this build, on three of Apple's own AUs (no licence of any kind):

| | AUDelay (own rack) | AUMatrixReverb | AUMultibandCompressor | AUDelay (a Link's live view) |
|---|---|---|---|---|
| opens without the plugin's own draw throwing | ok | ok | ok | **ok** |
| hands off 10 s: no frame, no layer, **not one draw** | ok | ok | ok | **ok** |
| the view draws when told to (the instrument) | ok | ok | ok | ok |
| a drag grips a knob | — | — | — | — |
| the copy's own values stay still, hands off | | | | **ok** |
| the tick inside its 50 ms period | | | | **ok** (2.80 / 1.97 ms) |

**The drag row could not run, and it says so rather than passing.** Nine synthetic drags over each
view moved no parameter at all: a posted mouse event does not get hold of an Apple AU's control in a
process that is not the front application. So that row SKIPS with that sentence - it is my
instrument, not a verdict about the picture - and **your own hand on step 6 is what answers it.**
An earlier cut of the row reported those as four FAILS, which would have been a fault invented by the
test; it now has to see a parameter move before it will judge the picture at all.
