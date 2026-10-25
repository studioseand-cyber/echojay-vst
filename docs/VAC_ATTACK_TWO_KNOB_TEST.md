# Two knobs, two names: which index really reduces gain (Mixland Vac Attack)

Use this only if B's check of Kathy's certified profile does not answer it. Five minutes, no
agent run, no chat — just two gestures and the log.

## What we already know, so you are not re-deriving it

Our map for this plugin is **`"origin": "human"`** (`~/Library/EchoJay/param_maps.json`, fp
`ff0867188e6a`), and it names four controls in the same 0–10 range:

| index | the map's name |
|---|---|
| 6 | **Left Reduction** |
| 7 | Right Gain |
| 9 | **Left Gain** |
| 13 | **Right Reduction** |

On 25 Oct at 17:23 the agent wrote **0.300 to 6 and 13** and you watched **both GAIN knobs move
to 3**. So the suspicion is a straight swap: 6 is the panel's left gain and 9 its left reduction.
That is a guess until the log says it, which is what this test is for.

## The test

1. Open the V2 on the Vac Attack's Link rack and open the **live view** on that slot. Have the
   plugin's own window up beside it so you can see both GAIN knobs and both REDUCTION knobs.
2. **No agent run.** If the rack strip says an agent is working on it, that is a regression in this
   build and I want to know — the shield should be clear.
3. Turn the live view's knob **labelled "Left Reduction"** a long way, in one movement. Watch the
   plugin's panel: **which knob moves — GAIN or REDUCTION?**
4. Turn the live view's knob **labelled "Left Gain"** a long way, in one movement. Again: **which
   panel knob moves?**
5. Leave the rack alone for a moment, then send me the two answers.

## What the log will say, and why I want it as well as your eyes

Each gesture prints its own line with the name AND the index:

```
EJControl: slot 1 "Left Reduction" set to 0.3000 ("3.0") from the V2's live view, gesture N
```

Two gestures, two names — and your two answers say which panel knob each one actually turned.
That is the pairing, from the product's own log rather than from my inference.

**If the refusal line appears instead** —

```
EJControl: a move on the live view was NOT sent - ...
```

— send me that line: it means the gate refused the gesture and the test did not run. (It is said
once per reason, which is why your 17:25 attempt left no trace at all; the next round adds the
count of how many moves a standing refusal has swallowed.)

## What happens with your answer

* If "Left Reduction" turns the GAIN knob: our map's names are attached to the wrong indices, and
  I swap them (6↔9, 13↔7) and re-certify the map against this evidence. The plugin is not at
  fault and neither is the agent — a hand-made map was wrong and every name-based road inherited
  it.
* If "Left Reduction" turns the REDUCTION knob: the map is right, and what moved on 25 Oct was
  something else — then I go back to the apply road with that, because the two cannot both be true.

Either way, until the map is corrected and re-certified, the trust file keeps this plugin off
name-based compression requests, so B dials on playback (which **measures** gain reduction) rather
than trusting a name.
