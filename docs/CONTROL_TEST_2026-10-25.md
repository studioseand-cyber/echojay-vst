# Control in the live view: the test, click by click (25 Oct 2026)

Build: **control ON** for parameters. Two faults were fixed first, and both of them are in this
build, because neither of them is safe to test without the other:

| | what it was | what it is now |
|---|---|---|
| the born-here hole | a picker-added slot's close-apply committed the V2's copy, and a control change never reaches that copy - so leaving the rack wiped the change he had just heard | once a control change has reached the Link for a slot, the Link is the newer side: the commit is **skipped** and says `LEAVE-LINK-NEWER` |
| the reconnect | opening the V2 after the Link's window closed left the strip greyed on "Not connected to this rack." until he switched racks and came back | a window open on a Link's rack **asks to engage once**, which is what the first window on a rack always did |

---

## Test 1 - control survives the close (the one that matters)

**On a copy of a session**, because this test deliberately leaves settings behind.

1. Open the V2 on Stereo Out. Select **Aitch_4_01**'s rack in the strip.
   *Expect: the strip connects by itself - no greying, no "Not connected to this rack."*
2. Add the **Tube-Tech CL 1B** with the + picker.
3. Click the slot to open its live view.
   *Expect: the panel shows the CL 1B's own interface, and there is **no** "View only" shield.*
4. **Turn Threshold by hand**, a long way, and listen.
   *Expect: you hear it change - the gesture goes to the real plugin on the Link.*
5. Close the V2's window.
6. Open the **Link's own window** on Aitch_4_01.
   *Expect: **Threshold is where you left it.** Not default, not the old value.*

That is the whole fault, closed. If Threshold is back at default, stop and send me the log: the
line to look for is `EJApply: slot 1 "Tube-Tech CL 1B" ... controlLanded=Y -> ... action=LEAVE-LINK-NEWER`.
If it says `action=COMMIT` the rule did not see the change; if there is no `EJApply` line at all,
nothing tried to write and the loss is somewhere else.

## Test 2 - the same for a slot that was already on the Link

1. Leave the CL 1B where it is. Close the V2, reopen it, select Aitch_4_01 again.
   *(This time the slot comes from the Link's own rack, not from your picker add.)*
2. Open its live view, turn **Gain** by hand, listen.
3. Close the V2, open the Link's window.
   *Expect: Gain is where you left it.*

## Test 3 - the reconnect, on its own

1. With the V2 open on Aitch_4_01, open the **Link's own window** (the V2's window closes).
2. Change something on the Link's window, then **close it**.
3. Open the V2.
   *Expect: the rack strip connects on its own within a second or two - no greying, and no
   "Applied to Aitch_4_01 ..." banner left in the middle of the page.*
   *Do **not** switch racks. That was the old workaround and it still works, which is why the fault
   was easy to miss.*

## Test 4 - control is refused with a reason, never dropped

1. Ask the agent to set something on that rack, and while it is working, turn a knob in the live
   view.
   *Expect: the shield is back, saying "an agent is working on this rack - stop it, or wait".*
   *Not expected: the knob moving and nothing happening.*

---

### What is still deliberately not control
A change that is not a parameter - a preset menu, a drawn curve, a file load - is not sent. The
button to open it on the Link is there for those. The shield only comes off for parameters, which
is exactly what your instruction asked for.
