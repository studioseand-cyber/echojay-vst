# ship_2026-09-21t-l - by-hand checks

Both bundles changed. It goes on top of **the installed 21t-k**, read off the AAX folder today rather than off a
note: V2 `49F30609` / `AA0C2737` and Link `E53D3A89` / `13D36AE3` are 21t-k's four UUIDs exactly. Nothing in this
folder has been signed, installed or pushed.

This round is the NINE things you found testing 21t-k this morning, plus the headroom op's apply. Each heading
carries your test number.

## TEST 1 - A GROUP STRIP SHOWS THE COUNT, NOT FOURTEEN NAMES

"Group 2" painted all fourteen member names down the data band, over the GROUP label and over the strip below it.
A group strip's body is now one line - **"14 Links"** (singular at one) - at every size, and it cannot grow past
its rect. The names are still there: hover the group row and the **tooltip lists them**, beside the roster
highlight that already shows the set.

## TEST 2 - THE GROUP FADER'S UNDO PUTS THE GROUP'S OWN FADER BACK

Undo restored every member but left the group's fader sitting at -3.1, and the log line named nobody
(`group ""`). Both come from the same cause - a group id being resolved through the Link name resolver. Now:

    EJUndo: group "the VCA" moved 3.00 dB (asked 3.00), fader -3.00 -> 0.00 dB

Move a group fader, undo, and the group fader reads **0.0** again with the members back where they were.

## TEST 3 - key_source TELLS THE TRUTH

A build whose key came from the CANDIDATES still read "from this channel (declared Mix Bus)" - the stamp put the
last [KEY] label onto whatever key arrived. The build's key, scale and reference must now all match what [KEY]
actually printed (reference within 0.2 Hz) to carry that label; anything else is stamped `chat`, which the plugin
reads as **"(from chat)"**. Two decisions I took that your ruling did not name: a field the build OMITS is not
counted against it, and a turn with NO [KEY] block stamps nothing at all rather than "chat".

## TEST 4 - correction_mode NEVER READS "custom" AFTER A SERVER BUILD

Your 10:48:08 line reproduced byte for byte on the old tree. Two separate things were in it:

  - **The mode was natural all along.** It applied and wrote its own 78.6 / 55 / 60; the retune that followed
    called the custom-mode path unconditionally and relabelled it. The label was wrong, not the state. The rung
    is now derived from the live values, and a payload carrying both applies the mode LAST.
  - **The one refusal was right but unhelpful.** `voice_type` was refused as "(not a number)". The build sent
    "alto"; the schema spells it `alto_tenor`. A choice param's refusal now names the choices:

        ignored voice_type "alto" is not one of: soprano, alto_tenor, low_male, instrument, bass

    That one is the server's to fix - the legal values are in the line.

## TEST 5 - A CALIBRATION BLOCK ON AN EDIT TURN WAITS FOR THE PLUGIN

10:45:07 the edit; 10:45:13 "block not usable - wire slot 1 ... rack has 0 slot(s)"; 10:45:15 the XLA-3 loaded.
The block was judged six seconds before the plugin existed. An edit that ADDS or REPLACES now takes the build
path's road and waits for the dial to settle, logging `edit settled (dial settled) -> N loop(s) started`. An edit
that only SETS a value does not wait, and a block naming a slot of a genuinely empty rack is still refused, with
the reason.

## TEST 6 - THE [KEY] BLOCK SENDS WHAT THE METERS PANEL IS SHOWING

The panel painted F major while the blocks went out as C 0.00, F 0.00, D minor 0.04, D major 0.17 - the panel
reads its 2 Hz cache and the block re-collected the detector at send time, two different instants of a
fluctuating reading. The block reads **the same struct the panel paints** now, with a fresh collect only if the
cache has never been filled. Panel and block agree on key AND confidence.

## TEST 7 - THE GROUP REPAIR WAITS FOR A LIVE LINK

It ran four seconds into the load, before any Link had registered, and reported "8 with a name but no live Link"
against an empty registry. It now runs on **the first tick a live Link appears**, never against an empty
registry, and gives up after 30 s of nothing.

## TEST 8 - "7 of 8 ... Main vocal 4 is still at -2.3 dB"

Observed first: that Link's heartbeats read -2.30 continuously for eleven seconds, so it was not a late readback.
The fan-out sent seven commands and never sent one to Main vocal 4 - it is the member whose rack this instance
had BORROWED, so its op went down the rack-apply road, which has no trim to move and aborted. A level match is
not a rack op: a borrowed member's trim is now written **directly**, and the summary says so:

    EJLevelMatch: "Main vocal 4" (93ffd6e0fe) is BORROWED by this instance - its trim is written directly
                  (-3.70 dB), because a level match is not a rack op

**One thing I owe you here:** the guard leg for this needs a live lease, which is a two-process rig. It is the
first item of the next round. The fix itself is in and exercised; the leg is not.

## TEST 9 - THE HEADROOM CAP IS THE MINIMUM ROOM ACROSS EVERY MEMBER

The code already took the minimum - the guard could not tell, because its one loud channel was also its tightest.
A second fixture channel (18 dB quieter, 2 dB hotter) now proves the offset comes from one member and the cap
from another, and the note NAMES the member that capped it:

    -6.0 dB [from the loudest member, Vox at -12.0 LUFS; capped to -6.0 dB by Stab's true peak]

## AND THE HEADROOM OP NOW MOVES TRIMS

In 21t-k it parsed and drew its card but wrote nothing. Both forms apply now:
`{"op":"headroom","mode":"relative","delta_db":-10}` writes the delta to every Link in scope; the target form
writes (target - the loudest member's SHORT max), capped by test 9's rule. One undo entry for the whole op, every
write verified by the 0.1 dB readback, and any member excluded or under 15 s of programme is named.

## UNDO IN THE LINK MIXER

Undo in the Link Mixer is the header undo button; Cmd-Z works too when Pro Tools passes it through, and the button's tooltip says which is which.

(The route for Cmd-Z is unconditional in this plugin, but Pro Tools binds Cmd-Z to its own session undo and only
forwards what it chooses to. That is the host's decision, so the tooltip stops promising it:
`Undo Link trim BV 1 - this button always works; Cmd-Z does when the host passes it through`.)

## HOUSEKEEPING

  - **own_diff is GREEN again**, re-baselined on your ruling: the 65 added lines were the built-in device
    descriptions entering the feed, nothing removed. Baseline is `OWN_CHANNEL_BASELINE_c7b142a.txt` in the repo.
  - The `classify_body_guard` flake was re-ruled, not patched: it had been asserting that your LIVE
    ~/Documents/EchoJay dump was untouched across its run - a file the shipping product writes on every chat send
    while your Pro Tools is up. It now asserts what it owes, that THIS RUN did not write it.
  - Still outstanding from 21t-k, if you have a moment: the MC 77 profile probe for the Input law.

        P="/Library/Application Support/Avid/Audio/Plug-Ins/EchoJay V2.aaxplugin/Contents/MacOS/EchoJayProbe"
        "$P" <the MC 77's AU id> --text-at <Input's index> > ~/echojay-vst/tools/loudness_loop_guard/fixtures/mc77_input_text_at.txt
