# ship_2026-09-21t-h — by-hand checks after installing

A hotfix on top of the installed 21t-g, from Sean's 27 Sep session. Four defects, and how to see each one is gone.

## 1. The compressor's threshold is actually set now (the 10:15 defect)
**What happened:** on a build to a channel whose rack EchoJay is holding, the log said
`EJCalib: block not usable - slot 0 is not in this rack (0 slot(s)); nothing started.` — the loop was started one
message-loop turn before the plugin had been added, so it read an empty rack and gave up. Nothing else in that build
was affected (the Attack and Release settings landed six seconds later, as the log shows).

- Play the song, ask for a compressor on a vocal channel.
- In the log, **after** the slot appears, expect
  `EJThreshold: leased build settled (dial settled) -> 1 loop(s) started`, then one
  `EJThreshold: "<plugin>" slot N PASSIVE, band 2.0-3.0 dB, dialling <knob> from <n> dB`.
- Then the threshold moving in 1 dB steps, at most six, and **one** closing line if it moved:
  `Adjusted the <plugin> <knob> to N dB.` No card, no "Listening", nothing asking you to play anything.
- **If you ever see "not in this rack" again**, the line now prints the wire value:
  `wire slot 0 (1-based, so slot index -1), rack has 1 slot(s)` — that is a server/client numbering disagreement,
  not an empty rack, and it says so at a glance.

## 2. "Level these channels" works on a chat turn, not only on a build
**What happened:** the `<<<ECHOJAY_LEVEL_MATCH>>>` block arrived on the chat route and was printed to you as raw
JSON with "(sent as a chat, not a build - say 'build' to build)" under it.

- On a group, ask to level the channels **without** saying "build".
- Expect a card, not JSON: `Level match, N channels:` and one line per member —
  `Main vocal: +1.4 dB (INT -19.4)`, and `no signal - left alone` for a member with no reading.
- Press Apply: the trims move exactly as they do from a build card, and the bubble says how many were matched.
- **No `<<<ECHOJAY_...>>>` text should ever appear in a bubble.** If it does, that route is missing its extractor.

## 3. The Mix Bus no longer lights up with a group
**What happened:** selecting "Main vocals (7)" highlighted the seven members **and** the Mix Bus strip.

- Select a group on the Link tab. The members highlight; the **Mix Bus strip must not**.
- Clear the group: the Mix Bus lights again (it is the main context when no channel is selected — that part was
  always right).
- Worth knowing, because the screenshot showed it: selecting a group does **not** change the transcript. The pill
  reads "Working on Group: Main vocals (7)" while the conversation underneath is whichever one was already open — in
  the screenshot, a Mix Bus build. That is current behaviour, not a defect fixed here; say the word if the group
  should own its own transcript.

## 4. The log says what it does
`EJCalib:` is now **`EJThreshold:`** on every line. Nothing else changed about the lines; the loop's own header file
keeps its name.

## Not in this build
`linkmixer_test` — the Link mixer's geometry self-test — was **never in the guard suite**; it had its own script and
nothing ran it. It is registered now, and it immediately showed **one pre-existing failure**: the merged Active
control's centre is claimed by an earlier rect in the hit order (the failure now prints which one). That is a real
mixer-geometry fact, it predates this round, and it is not fixed here — fixing hit rects unattended, without seeing
the UI, is how a layout gets worse. It leads the next round.
