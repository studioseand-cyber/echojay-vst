# Morning report, 27 Oct 2026 — the real editor
### Written through the night. **Read §1 first: it says what is installed and what to do.**

## 1. State, in one paragraph

Sean is on **V2 AU `9B29E0FE` / Link AU `F95EC755`** — the build he tested green yesterday evening
(round 1 plus the transport-stopped fix). Whether the real editor was installed on top of it is
answered in §5, which is written last, from the final gate. The real-editor switch is **off by
default and hidden from Settings outside dev mode**, so a build carrying it behaves exactly like the
one he is using. The copy road was not touched.

## 2. What the re-homing is, and that it works

His approved change to §2: a lent editor **moves into the V2's window once and is kept**; when the
borrower lets go, the **lender takes its own view back** in one `addSubview` into a parking view it
owns — never a remove-then-add, never a hop through a nil window. The editor is destroyed only by
the teardowns that destroy what it is an editor of.

Measured, on Apple's AUDelay/AUMatrixReverb/AUMultibandCompressor and three licence-free
third-party plugins:

| what | row | result |
|---|---|---|
| the pane shows the LINK'S OWN editor, with no copy started at all | RE8 | green |
| five seconds hands-off: frame unchanged, pixels unchanged | RE9 | green |
| a knob moved on it is still on the Link's plugin after a close and reopen | RE10 | green |
| the V2's own output unaffected, against the noise floor of two view-free renders | RE11 | green |
| the slot's plugin replaced under a lent editor: the window lets go first | RE12 | green |
| the switch off puts the view back on the copy road, nothing else to undo | RE13 | green |
| hand-back to the Link's own window and back again, **three laps each way** | RE14, RE15 | green |
| **fifty opens and closes per plugin build exactly ONE editor** (one press = one take, and the leak test) | RH2 | green |
| every subject loads and holds its own panel through all fifty | RH1 | green |
| the slot deleted while shown: the window lets go of that plugin's editor | RH5 | green |
| the track deleted while shown | RH6 | green |
| the V2's window closed while shown, and the table sane afterwards | RH7, RH8 | green |

## 3. The subjects, and the one that is not in the set

Apple's three AUs, plus:

* **MCompressor** (MeldaProduction) — their own UI framework. *He asked for MTransient; it is not
  installed on this Mac. Same maker, same framework.*
* **ValhallaDelay** (Valhalla DSP) — the largest custom interface in the set.
* **TDR SlickEQ M** (Tokyo Dawn Labs) — **JUCE-built**.

**Fresh Air** (Slate Digital) was a fourth and is out: enumerating an AU identifier instantiates the
plugin, and Fresh Air never returns from instantiation. A standalone probe (one plugin per process,
25 s limit) was killed still waiting, while MCompressor took 209 ms, ValhallaDelay 128 ms, SlickEQ
67 ms and AUDelay 3 ms. It cost the rig a twelve-minute stall before it was measured, and the rule
is now in the code: an identifier joins the catalogue only after that probe answers for it.

## 4. A finding that is not ours, and he should know about it

**Deleting a track with ValhallaDelay hosted on an EchoJay rack takes the process down**, inside the
plugin's own thread:

```
EXC_BAD_ACCESS (SIGSEGV) at 0x0 — pthread_mutex_lock
    ValhallaDelay  ×3        (its own thread; no EchoJay frame in the stack)
```

It happens **identically with the real-editor switch off**, on the copy road — i.e. on the build he
is running today. Measured deliberately (`EJ_RH_COPY_ROAD=1`), same exception, same frames. Two crash
reports are in `~/Library/Logs/DiagnosticReports` (23:50:03 and 23:56:54). Taking it off the rack does
not help: a parked instance lives until the processor does. Nothing in the live-view work causes it,
and nothing in the live-view work can fix it; it belongs on the queue as its own item.

Consequence for the suite: the rig now prints its verdict **before** tearing the tracks down. Four
passes had reported "47 rows, 0 verdicts" while every row in them had already been decided, because
that plugin's shutdown crash ate the result.

## 5. The gate, the install, and what to test

**NOTHING WAS INSTALLED.** You are still on **V2 AU `9B29E0FE` / Link AU `F95EC755`**, the build you
tested green yesterday evening. Your rule was "install only when every one of those, every live-view
row and the RE rows run green with none skipped", and one row is red. The switch is off and hidden,
so nothing of this would change that build anyway.

### The final gate (V2 `A5B511C9`-lineage build, rebuilt through the night)

```
chat_store_guard                         GREEN   357 assertions
host_rig live-view + RE + RH             RED     90 passed, 1 failed, 0 SKIPPED
```

Green: LV1-LV11 (the whole copy road), RE0-RE15 (directory, lease, exclusivity, the real editor
shown with no copy, hands-off stillness in pixels, a knob that stays on the Link's plugin across a
close and reopen, the V2's output unaffected, slot replace, the switch putting it back, and the
hand-back three laps each way), RH1, RH2, RH4-RH8.

**The one red: RH3, "switching to another slot and back brings the same plugin's own editor back."**

* It **passes on its own**: `EJ_RIG_ONLY=RH1,RH3` → *first "AUDelay" (road "real"), the other slot
  left the road on "real", back to "AUDelay"*. The row is sound and the product does the right thing.
* In the full pass it fails because the open takes the **copy** road, and the pane's own reason is
  "the real-editor switch is off" - **moments after the row verified, through the plugin's own road,
  that the switch read ON** (the row presses up to ten times and prints a line if it will not stay;
  that line did not appear). Those two readings contradict each other, in one process, seconds apart.
* That is the open question, and it is mine, not yours: either the press is not reaching the pane's
  copy of the decision, or something between the RE rows and the RH rows turns it off. It is a test's
  precondition rather than a user road - you do not flip that switch fifty times a minute - but it is
  not proven and so nothing is installed.

### What I would do next, in order

1. Find the switch-state contradiction (log both readings with their timestamps inside one open).
2. Re-run the gate; install only on a clean sweep.
3. Then your click-by-click, under supervision, including the CL 1B and the Vac Attack.

### The click-by-click, ready for when it does install

With the switch ON (Settings, dev builds only for now), on a Link's rack:

1. Click a slot card. **Expect the plugin's own window inside the V2** - the real one, no copy. The
   log says `EJPane: showing the LINK'S OWN editor for slot N of <uid>`.
2. Hands off for ten seconds: nothing moves. (This is the fault you reported on 26 Oct as "wobble".)
3. Drag a knob: it follows your hand, and the change is on the Link - close the V2 and reopen it, the
   value is still there, because it never was a copy.
4. Open the same plugin on the **Link's own window**: the V2 lets go and the Link's window shows it.
   Press the V2's card again: it comes back. Do that a few times.
5. Delete the slot while it is showing: the V2 lets go rather than drawing a dead panel.
6. Turn the switch OFF: the view goes back to the copy road at once, and everything you tested
   yesterday behaves as it did.
7. The CL 1B and the Vac Attack: both have large interfaces and the Vac Attack has the map we fixed.
   Watch for a knob that does not follow, and for the window moving on its own.
