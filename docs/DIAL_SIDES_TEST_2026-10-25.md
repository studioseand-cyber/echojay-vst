# Click-by-click: the Vac Attack dial — both sides, B's words, no question

Build: the sides-and-words round (translator passthrough in `blockFromDialOnPlayback`).
What changed: B's `controls`, `say` and — new in this round — `min_db`/`max_db` now travel from
the dial block into the calibration block. They were being dropped, which is why **only "Left
Reduction" moved** on your 08:43 run and why the done line was the build's own sentence with a
question after it.

Logic must be closed for the install only; everything below is with Logic open.

---

## A. Both sides move

1. Open Logic, put the **Vac Attack** on a channel through EchoJay (either the Link's own rack or
   a Link's rack through the V2 — the road is the same translator).
2. Open the Vac Attack's own window so you can see **Left Reduction** and **Right Reduction** at
   once, and note both numbers before you start.
3. Start playback on material that is actually hitting it.
4. Ask the agent for compression on it in your own words (e.g. *"give the Vac Attack about 3 dB"*).
5. **Watch both numbers.** PASS = both Left and Right Reduction move, together, in the same
   direction. FAIL = one side moves and the other sits where it was (the 08:43 fault).
6. Before the dial starts, the log says in one line which sides it is about to move:

   ```
   EJThreshold: slot N is DIALLED ON PLAYBACK - the control(s) that will move:
   "Left Reduction", "Right Reduction"; the done line is B's own, from `say`. ...
   ```

   If the dial misbehaves again, that one line says whether the build was told about both sides or
   only one — which is the question I could not answer from the log this morning.

## B. B's words, and no question after them

7. Let the dial finish (it holds for ~8 s and at least 10 blocks, up to 4 corrections).
8. Read the line that lands on the agent's card. PASS = it is **B's sentence** (his `say.reached`,
   `say.short` or `say.over` with the gain reduction filled in), and it **ends without a question**.
   FAIL = *"…Does that sit better with the rest of the mix?"* or any other question tacked on, or a
   sentence that is clearly the plugin's own phrasing rather than B's.
9. A dial that fell short must say so: if it ran out of travel, the line is B's `short` wording with
   his end-word (e.g. "hardest"), not a success sentence about a number five times under target.

## C. The range — NOT IN THIS BUILD, waiting on B's contract

10. Nothing to test here yet, and nothing in this build reads a range from B.

    Sean's ruling of 25 Oct: B will send **min / max plus a unit, per control** — the control's own
    range in its own units (the Vac Attack's Reduction is **0 to 10, unitless**), not `min_db` /
    `max_db`. That is the remaining suspect for your Reduction ending at **+0.0**: with no range the
    loop walks every control inside **-60 … +12 dB**, which is not the space a 0-to-10 unitless
    control lives in, so "+0.0 dB" was the loop clamping in a space the control does not have.

    This waits for B's contract change before it is built: a per-control range **with a unit** is a
    different shape from today's two dB numbers, so it is a round of its own and not a passthrough.
    Today's defaults stand untouched until then.

## D. One control still moves one (nothing widened)

11. Repeat A with a plugin whose dial B describes with a **single** control (any mono compressor he
    profiles that way). PASS = exactly that one control moves, and the log's line reads
    *"B named no `controls`, so this is the only side"*. This is the leg that proves the fix added
    sides where B named them and nowhere else.

---

## What to send back

* A: did both sides move? (and the log's "control(s) that will move" line)
* B: the exact sentence on the card, and whether anything followed it
* C: nothing to send - the range waits for B's contract (min/max + unit, per control)
* D: one control still one?
