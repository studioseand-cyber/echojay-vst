# Slot meters: the test, click by click (25 Oct 2026)

**Where they are on the card.** The slot tile is 118 x 64 and every band of it was already taken -
the name and the IN / OUT **trim controls** across the top, the wet knob in the middle, the trim and
badge caption under it, the B / X / < / > buttons at the bottom. The one unused space is the wet
knob's own flanks, and that is where these went:

```
  +--------------------------------------+
  |  Tube-Tech CL 1B          IN   +0.0  |   <- the name, and the TRIM controls (unchanged)
  |                           OUT  -2.0  |
  |  IN  ====----  -18.5    ( )      GR  |   <- the METERS: in/out left, the third figure right
  |  OUT ======--  -12.5             4.2 |
  |  -2.3 dB match            GR 4.2 HOT |   <- the trim text and badge (unchanged)
  |  [B] [X]                   [<]  [>]  |
  +--------------------------------------+
```

The two things that both say IN are deliberately far apart and look different: the **trim** is a
control you drag, top right, no bar. The **meter** is measured, left of the knob, with a bar.

## Test 1 - they are live, and they follow the audio

1. Open the V2 on Stereo Out, select **Aitch_4_01**'s rack (it connects by itself now).
2. Play the track.
   *Expect: IN and OUT move on every slot card that is in circuit, within about a tenth of a second.*
3. Stop the transport.
   *Expect: the figures **fall** to silence over a second or so and stay low. They must not freeze at
   the last loud number.*

## Test 2 - silence is silence; nothing measured is nothing

1. With the transport stopped from the start of a session, look at a slot nothing has played through.
   *Expect: **no figures at all** - not "0.0", not "-200.0". A dash where a number would be.*
2. Now play, then stop.
   *Expect: figures, low. The difference between the two states is the whole point: a blank card
   means nothing has been measured, a low card means it is quiet.*

## Test 3 - the Link stopping blanks the cards

1. Play, so the cards are live.
2. **Quit the Link's track** (or disable that channel strip's EchoJay Link).
   *Expect: within half a second the figures **blank**. Not frozen, not zero.*

## Test 4 - the third figure, and what it is allowed to be called

1. On the **Tube-Tech CL 1B** (a third party, no certified map): the right-hand figure is headed
   **Δ** and is the difference across the slot.
   *Expect: Δ, never "GR". On a unit making up gain it reads positive, which is correct and is
   exactly why it is not called gain reduction.*
2. On an **EchoJay device** slot (or a third party whose map is certified): the figure is headed
   **GR** and matches that device's own meter.

## Test 5 - nothing on the card moved

1. Drag the **IN trim** on a card (top right), double-click to type a value.
   *Expect: it behaves exactly as before - the meters never take a click.*
2. Drag the wet knob, press B, press X, click the tile to select it.
   *Expect: all unchanged. The meters sit beside the knob and intercept no mouse at all.*

---

### Also in this build
**The save-time log line** you approved: the Link now logs the slot count **in the chunk it is
writing**, beside the count in the model it wrote it from, and flags them when they differ. So the
next time a rack comes back empty, the save side can be compared with
`setState ... chain=N slot(s) in the chunk` instead of inferred.

### What is not in this build
The S6 B6 "chip0" diagnosis - it is next, and it is read-only, so it needs nothing from you.
