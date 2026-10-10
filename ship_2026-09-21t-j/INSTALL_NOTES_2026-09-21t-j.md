# ship_2026-09-21t-j - by-hand checks

Both bundles changed. It goes on top of **the installed 21t-i**, read off the AAX folder rather than off a note:
V2 `453957B5` / `7A302436` and Link `DAF8007A` / `7DEB0AD8` are 21t-i's four UUIDs exactly, so 21t-i is what is
running now. (My own round notes said 21t-i was never signed; the disk says otherwise and the disk wins.)
Nothing in this folder has been signed, installed or pushed.

## 1. THE SETTLE: one line per build, finished in place

Build a compressor on a channel that has been played, and play the track.

  - The moment the build lands you get ONE chat line:
      `Purple Audio MC 77 on, set from 84 s of this track, landing it as it plays...`
  - **That same line is then rewritten in place** - not a second message - once the slot has heard the vocal and
    the setting has landed:
      `Purple Audio MC 77 on, set from 84 s of this track. Doing about 2.4 dB of gain reduction. Output trimmed
       1.8 dB to hold the level. Say 'ease off' or 'more'.`
  - Between the two, the knob may move **at most three times, inside 15 s of HEARD audio**. Stop the transport
    mid-build and the settle simply pauses: heard time counts, clock time does not. Play again and it finishes.
  - **After it lands, nothing moves on its own.** Say "ease off" or "more" and it moves ONCE, then the line is
    completed again with the new figure. One word, one move - if you ever see two moves for one word, that is the
    defect this round closed and I want the log.
  - Edit the slot yourself (or remove the plugin) while it is still settling: the line closes with the setting as
    it stands rather than promising to land something nobody is landing.

## 2. NO FIGURE BELOW THE GATE (the empty-bus strip)

  - Find a strip with nothing routed through it - the VOCALS bus you showed me read `MOM -85.6, SHORT -85.6,
    PSR 6.7` in amber.
  - Every one of those now draws `--`: MOM, SHORT, INT, and PSR / PLR / LRA, because a ratio or a range taken
    from a gated-out loudness figure is not a reading either. The line is the **-70 LUFS absolute gate**.
  - A strip with programme on it is unchanged: check one, and all six cells should still print.

## 3. GROUPS OF 64, AND NOTHING DROPPED SILENTLY

  - Select 22 strips and group them. The group holds **22** (it held 16, with nothing said), and the
    `[GROUP LEVELS]` block carries 22 member lines.
  - Select more than 64 and the dialog and the chat card **name every member left out**, with the count and the
    limit. The cap is 64 members per group and 16 groups.

## 4. A COPIED INSERT KEEPS ITS ROLE

  - Set a Link's role with the picker (bus or channel), then **copy that insert to another track** (or duplicate
    the track).
  - The copy appears in the Link Mixer with the SAME role, without you touching its picker - it used to arrive
    with no role at all, and the roster counted it "unset".
  - In the log: `EJLinkState: registry snapshot (registered|state restored|heartbeat|the picker was set) uid=...
    role=bus trim=-2.00 dB active=1`. Every publish says what it published and why.
  - Move a Link's trim from V2 and leave it: the heartbeat must NOT put the old value back. The row keeps yours.

## 5. THE KEY AND THE REFERENCE SAY WHERE THEY CAME FROM

  - With a key source selected (a capture, or a Link on the music bus), ask for a tuner build.
  - The tuner panel's key and reference lines now read `... from <that source>` instead of `(by hand)`.
  - On the wire the tuner slot line gains ONE field at the END, and only when a build attributed it:
    `correction_mode hard, retune 0.0, flex 0, humanize 0, key F#, scale minor, ref 441.0 Hz, key_source "..."`.
    Change the key yourself afterwards and the attribution disappears, because the value is no longer that
    source's.

## 6. THE GR-METER CROSS-CHECK - one action, for the MC 77

B's block says the MC 77's "GR Meter L" reads as dB. This build writes down what it actually prints.

  - Build a compressor on a Link with the MC 77 in it and let it play about five windows (15 s).
  - Then, in Terminal:
        log show --last 10m --predicate 'eventMessage CONTAINS "EJGrMeter"'
  - Five lines, in this shape:
        EJGrMeter: Purple Audio MC 77 "GR Meter L" window 1/5 raw=0.4100 text="4.2 dB" parsed=4.20 dB prints_db=yes
    If the text is a bare number the line says so and adds `NOTE: the parsed figure equals the raw position -
    this control may not be printing dB`. That answers the question either way.

## 7. THE MC 77 PROFILE PROBE - one action, and where the fixture lands

ONE ACTION, after installing, in Terminal (the probe ships inside the bundle, so no separate download):

    P="/Library/Application Support/Avid/Audio/Plug-Ins/EchoJay V2.aaxplugin/Contents/MacOS/EchoJayProbe"
    "$P" <the MC 77's AU id> --text-at <Input's index> > ~/echojay-vst/tools/loudness_loop_guard/fixtures/mc77_input_text_at.txt

THE FIXTURE LANDS at `tools/loudness_loop_guard/fixtures/mc77_input_text_at.txt` in the repo, beside
`calibration_blocks_from_contract.txt`, which is where the other side's own text already lives. Send me the path
and I will read the curve off it; nothing here reads it automatically.

WHAT IS IN IT: **21 rows** now (normalised 0.00 to 1.00 at 0.05), or one row per detent on a stepped control,
instead of the three the old probe took. The shape is unchanged - the same `at<TAB><norm><TAB><text>` rows, only
more of them - so B needs no change to read it. If you do not know Input's index, run the same command with
`--list-params` first and read it off the list.

## WHAT IS NOT IN THIS BUILD

  - `pitch_mode_test`'s seven pre-existing key-detection failures (21t-k item 8) - untouched, and the only red
    left in the gate.
  - The Link Mixer selection work, undo, the group fader as a VCA, strip labels, the headroom op and scope-by-role
    are all 21t-k, as ruled.
