# The real editor: your click-by-click (27 Oct 2026)

The switch is **off** in the build and **hidden from Settings**, as you asked. It is one file, read fresh
every time a slot opens, so nothing needs restarting to turn it on or off:

    on :  mkdir -p ~/Library/EchoJay && touch ~/Library/EchoJay/real_editor_live_view
    off:  rm -f ~/Library/EchoJay/real_editor_live_view

(Type either with a leading `!` in this session and I will run it here.)

Everything in part 1 is the **copy road** - the live view you have been using. Do it with the switch off
first: if any of it has changed, stop there and tell me, because the copy road was not supposed to move.

## Part 1 - the copy road, switch OFF (about 4 minutes)

1. One track with EchoJay V2, one with EchoJay Link. On the Link's rack: **Tube-Tech CL 1B**, then
   **AUDelay**, then **Vac Attack**.
2. In the V2, aim at the Link and open the **CL 1B**. Expect its own panel, no "not following" bar.
3. Drag Threshold. Expect the view to follow smoothly, and the Link's own CL 1B to move with it.
4. Close the slot, reopen it. The value you set is still there.
5. Open **AUDelay**, then the **CL 1B** again. The right panel each time, no leftover from the other.
6. Delete slot 2 on the Link. The Link's window for it closes; the V2's view does not show a dead panel.

## Part 2 - the real editor, switch ON (about 8 minutes)

Turn the switch on with the line above. Nothing to restart.

7. Open the **CL 1B** in the V2's live view again. Expect: the real panel, and the strip line reading
   *"Showing Tube-Tech CL 1B's own window, live on the Link"*. No copy is started at all on this road.
8. Hands off it for ten seconds. Nothing should move, wobble or re-draw.
9. Drag Threshold. It is the Link's own plugin, so the Link's own window shows the same move if you have it
   open. Watch for any judder.
10. **Open and close it ten times** (double-click the card, then click another card, then back). The panel
    must be the CL 1B's every time, and no slower on the tenth than the first.
11. **Switch slots:** CL 1B -> AUDelay -> CL 1B. The right panel each time.
12. **The same slot number on your own rack:** put anything on the V2's own slot 1, open it, then aim back
    at the Link and open ITS slot 1. You must get the Link's plugin, not your own. (This is the one that was
    broken until this morning; rig row RH9 and guard legs IV0-IV6 now hold it.)
13. **Hand-back, both ways:** with the CL 1B showing in the V2, open the same plugin from the **Link's own
    window** in Logic. The V2 lets go, the Link's window shows it. Then open it in the V2 again. Three laps.
14. **Replace the slot's plugin** on the Link while the V2 is showing it: the V2 must redraw to the new
    plugin, never keep the old panel.
15. **Delete the slot** while it is showing. Nothing left behind, no crash.
16. **Close the V2's window** while it is showing, then reopen it. Open the CL 1B again: it still works.
17. **Vac Attack** in the V2's live view: open, drag a control, close, reopen. Its setting stays on the Link.
18. Switch the real editor **off** again with the line above. The next open goes back to the copy road and
    says so in the strip line. Your settings on the Link are untouched either way.

## What today's work changed, so you know what you are looking at

- **The real editor now opens from a plain card press.** Until this morning it only ever opened for a
  slot that already had an edit session engaged on that Link - so with the switch on and no session, the
  pane opened nothing at all. Step 7 is the step that would have failed.
- **Turning the switch off puts the copy back on screen by itself.** It used to hand the lent editor back
  and leave the pane empty until you pressed something.
- **A slot number no longer stands for a rack.** Your own slot 1 and a Link's slot 1 are different things
  to the pane now (step 12).
- **Nothing about the copy road changed.** With the switch off, every one of those decisions answers "no"
  and the code is yesterday's. Part 1 is there to prove that, and it is the part I most want you to check.

## What I want to hear back

- Anything in **part 1** that is different from yesterday (that would be a regression, not a new feature).
- For part 2: any wobble, any wrong panel, any pause longer than a second, and whether step 13 ever leaves
  you with two windows thinking they have it.
- If anything crashes, don't clean up - tell me and leave the report where it lands.

## One thing that is known and is not ours

Deleting a track with **ValhallaDelay** on it can take Logic's plugin host down **at quit**, inside
Valhalla's own thread, while the process is shutting down. It happens identically with the switch off and on
a bare host with no EchoJay in it at all; the write-up is `docs/VALHALLA_EXIT_CRASH_2026-10-27.md`. Not
something to chase during this test.
