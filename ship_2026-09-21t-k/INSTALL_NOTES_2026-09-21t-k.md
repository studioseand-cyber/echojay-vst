# ship_2026-09-21t-k - by-hand checks

Both bundles changed. It goes on top of **the installed 21t-j**, read off the AAX folder tonight rather than off a
note: V2 `667B3D46` / `9C359AF2` and Link `2D14A17D` / `85E4270D` are 21t-j's four UUIDs exactly. (The folder
still held 21t-i when this round started; 21t-j was signed and installed during it.) Nothing in this folder has
been signed, installed or pushed.

## 1. A REOPENED SESSION KEEPS ITS LINKS - this is the big one

Your "Main vocals (7)" level match addressed seven dead uids because every Link re-minted its identity when the
session opened. That is fixed at the root: **a Link keeps its uid whenever no live slot holds it**, and mints a
new one only against a PROVEN-LIVE duplicate (a copied insert while the original is running).

  - Open a session with several Links, close it, open it again in the same Pro Tools. In the log:
      `EJLinkState: uid <id> is free (no slot holds it) - KEPT, so this Link is the same Link it was before the
       reopen`
    and NOT the old line, `came from a chunk authored in THIS host run ... -> regenerated`.
  - **The typed names survive too.** They used to be dropped with the uid ("seeded names dropped").
  - **Groups made before the reopen still work.** Select one and ask to balance it: every member should report
    figures, not "no signal".
  - COPY an insert while the original is running: the copy gets a NEW uid (two Links cannot share an identity)
    and **keeps the name it was copied with**. Rename it if you want them different.
  - Your two CURRENT groups ("Main vocals", "All channels") were made before names were stored, so the repair has
    nothing to match on - re-make them once. Every group made from now on repairs itself on load, logged as
    `EJGroupRepair:`.
  - A member that genuinely is not in the session now reads **"<name> - not in this session"**, never a raw uid.

## 2. THE TUNER SAYS WHERE THE KEY CAME FROM - on a Link build too

  - With a key source selected, build a tuner on a LINK (through the session host, as you did at 21:24). The
    readback line should now END with `, key_source "<the source>"`, and the plugin should read
    **"key F major from the Mix Bus"** rather than "(by hand)".
  - `EchoJay Pitch` (and the EQ, Level, Limiter, Compressor, Key Detector) are now in the feed on every send, so
    `EJChat: chain name OUT OF FEED: "EchoJay Pitch"` should not appear again.
  - A weak key reading is no longer sent as gospel: below 0.2 confidence the [KEY] block shows the LAST STABLE
    reading and adds one line -
      `stability: unstable (live 0.06 below 0.2) - values above are the last stable reading, confidence 0.47,
       age 45 s`

## 3. THE COMPRESSOR BUILD: no more 20 dB misses reported as 0.0 dB

  - The Zip build wrote -15 onto EchoJay's own staging because the block said `actuator=drive` AND named
    `Threshold`. That block is now REFUSED: the start is ignored, the drive opens at the staging, and the log says
      `a drive block carries a param and a start - contradictory`
  - The settle gets **3 steps, each judged on 2 fresh windows**, with a 45 s heard ceiling - the old 15 s cap
    stopped it after ONE step.
  - **The level sensor measures the PLUGIN now**, not EchoJay's staging: a slot with pre -15 and a unity plugin
    reads a level change of 0, where it used to read "15.0 dB quieter out than in".
  - And the reply reports what the hold WROTE: `Output +12.0 dB, its limit - the slot is still 3.0 dB down and I
    cannot hold the rest.` It used to claim 15.0 dB while the control had stopped at 12.
  - The window line now prints both heard figures - `heard=15.1s (slot 18.1s)` - so the jump you saw from 0.0 to
    15.1 reads as what it is.

## 4. THE LINK MIXER

  - **Shift-click** selects the run from the last plain click; cmd-click still toggles; a shift-click with no
    anchor yet behaves as a plain click.
  - Right-click a strip: **Select all channels (N) / Select all buses (N) / Select all (N) / Select none**.
  - **Double-click a fader** to reset it to 0.0 dB - and cmd-Z undoes it.
  - **The group fader is a VCA**: one gesture is ONE undo step, and undo puts the members back while they keep
    their own offsets.
  - Long names: one line, ellipsis, and the strip's **index number** when even the ellipsis would leave fewer
    than four characters. Nothing overflows.
  - A Link still registering is REFUSED from a group, by name, instead of being dropped silently.

## 5. NO FIGURE BELOW THE GATE (shipped in 21t-j, which you now have - worth a glance anyway)

Every loudness figure the UI draws - MOM, SHORT, INT, and PSR / PLR / LRA taken from them - draws `--` below
-70 LUFS. Your empty VOCALS bus should show no numbers at all now.

## 6. THE HEADROOM OP is parsed but does not move anything yet

`{"op":"headroom","mode":"relative","delta_db":-10}` and the target form parse, resolve their scope
(`{"scope":{"role":"channel"}}`) and draw their card row with the count. **Applying** the offset to the trims is
the next cut - nothing here writes a trim, so an unapplied headroom card cannot move your mix.

## 7. ONE ACTION FOR ME, when you have a moment

The MC 77 profile probe, for the Input law:

    P="/Library/Application Support/Avid/Audio/Plug-Ins/EchoJay V2.aaxplugin/Contents/MacOS/EchoJayProbe"
    "$P" <the MC 77's AU id> --text-at <Input's index> > ~/echojay-vst/tools/loudness_loop_guard/fixtures/mc77_input_text_at.txt

21 rows now (0.00 to 1.00 at 0.05), or one per detent on a stepped control. Same row shape as before.

## AND ONE THING I DID NOT DO

`own_diff` is RED in the gate and I left it that way on purpose: your rule is "if own_diff goes RED, stop and
report, do not re-baseline". What moved is 65 lines ADDED and none removed - with the built-ins in the feed, a
machine with no scanned plugins now carries [AVAILABLE PLUGINS], the chain-block rule and the built-in
descriptions, where before it carried none of them. The new text is in the repo as
`OWN_CHANNEL_CANDIDATE_21t-k_<sha>.txt` so you can diff it in one step and rule on it.
