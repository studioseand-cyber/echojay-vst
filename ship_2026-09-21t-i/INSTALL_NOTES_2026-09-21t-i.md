# ship_2026-09-21t-i - by-hand checks

Installed on top of 21t-h, which IS the pair in the AAX folder right now: V2 x86_64 F1F21673, Link 56E0EAF3,
signed 27 Sep at 14:31 (read off the installed bundles, not from a note - my earlier text said 21t-h had never been
signed, and the disk says otherwise). Both bundles changed.

## 1. The compressor is SET ONCE and then reports - no automatic stepping

Ask for a compressor on a channel that has been played for a while, build it, and play the track.

  - WATCH THE PLUGIN'S KNOB: it is set once, at the build, and it does not move again on its own. Before this round
    the loop walked it a dB at a time up to six times.
  - After about two windows of playback (6-7 s) ONE chat line appears, in this shape:
      NEOLD U2A is on, set from 84 s of this track, doing about 5.4 dB of gain reduction on the loud phrases.
      How's that sounding? Say 'ease off' or 'more'.
  - Say "ease off" (or "more"). The server re-targets the band; the knob moves ONE step in that direction - the
    threshold by whatever step the block names (1 dB unless it says otherwise), the drive by 1 dB - and about two
    windows later it reports the new figure and asks again.
  - Nothing is heard within 30 s of the build: ONE line, once, "<plugin> is on - play it and I'll tell you what it
    is doing." No card, no "Listening...", and no closing line anywhere in this mode.
  - THE LOG (Console, or ~/Library/Logs/EchoJay): every judged window prints
      EJThreshold: "<plugin>" window N gr=2.3 pre=+0.0 post=-0.0 mode=passive state=measuring|asked|stepped
    A window that moves nothing still prints, so a stalled loop and a quiet in-band loop cannot look the same.
    A loop that CANNOT be advanced says so every 5 s, naming why.

## 2. The kept levels: a channel that has stopped playing still has figures

  - Play a few vocal channels through, one at a time, then STOP the transport entirely.
  - Select those channels as a group and ask to balance them. The [GROUP LEVELS] block now carries each member's
    INT, SHORTMAX, SHORT90, PEAK, PSR and HEARD - what it heard while it played - with an AGE token saying how old
    the figures are. Before this round the strips showed an INT and the block said those same channels had no
    signal.
  - "no signal" now means HEARD 0 and nothing else: a channel nobody has played, and only that.
  - SAVE THE SESSION, CLOSE IT, REOPEN IT. The figures are still there, without playing anything, from the Link's
    own saved state and this instance's. A Link that is quiet or parked is read from its rack sidecar.
  - TO CLEAR ONE: right-click its strip -> "Reset kept levels (N s heard)". That channel reads no signal until it is
    played again; every other channel is untouched. Asking in chat to reset the levels on a channel does the same
    thing (the server sends a reset_levels op).
  - THE AGE TOKEN, as ruled 27 Sep: "AGE <seconds>", an integer, after HEARD, on the [GROUP LEVELS] member lines
    and on [TRACK LEVELS]. It is NOT on the [CHAIN LEVELS] header. B parses it from its next deploy; until then it
    is one extra token at the end of a line, which its current parser ignores.

## 3. The level-match card, on the chat route, with its Apply button

  - Select a group and ask to balance it. If the reply comes back as a chat rather than a build, the card still
    renders - one line per member with its delta - AND it has an "Apply changes" button. Press it: the members'
    trims move and a line says how many took the change. Before this round the lines rendered with nothing to press.
  - The footer "(sent as a chat, not a build - say 'build' to build)" must NOT appear under a level-match card.
  - While the turn is in flight on a group, the placeholder reads "Working..." rather than "Answering as a chat...".

## 4. The re-cut items (27 Sep, evening)

  - THE SENTENCE QUOTES THE SERVER'S FIGURE. "set from 120 s of this track" is the block's own heard_s, not the
    slot's age. A compressor set from the working position says "set from the working position"; a block that says
    neither omits the clause rather than inventing a number.
  - "EASE OFF" / "MORE" NOW WORK EVEN WITH NOTHING TO EDIT. The server can answer a comparative with an empty ops
    list and a calibration carrying nudge "harder"/"softer"; the knob takes ONE step on the next judged window and
    the loop reports again. Watch the log for `EJThreshold: ops-free calibration block on the reply -> 1 loop(s)`.
  - THE TUNER LADDER: natural, balanced, hard, snap. "snap" is new - retune at the map's minimum with correction
    full, which on EchoJay's own tuner is where hard already sits. "tuned" still works and is taken as hard, and an
    unknown rung builds as hard and says so rather than leaving the tuner untouched.
  - Two things that were going out on [CURRENT CHAIN] are fixed: "correction_mode balanced1" and figures like
    "retune 33.8917".

## What is NOT in this build

  - A painted "reset levels" button on the strip face. The control is on the strip's right-click menu instead: a
    narrow strip is 46 px wide and the layout already drops controls that cannot fit. Say the word and it becomes a
    button next round.
  - The plugin picker over an inline editor: closing it with Esc does not yet bring back the editor it covered.
    Ruled low priority, not this round.
  - The per-tuner half of the snap ruling. This tree has no certified-tuner table and
    EJ_MAP_CERTIFICATION_2026-09-25.md is not on disk; third-party tuner controls are dialled from the server's
    settings_structured, so "snap" for a third-party tuner is a map/server matter. The client's own tuner is done.
  - Seven pre-existing failures in pitch_mode_test, all in key detection (a 0.31-confidence key falling back to
    chromatic, the chromatic mask, the state's fallback report). That test had never run in the gate; it does now,
    so they are visible. They need their own round and none of them is touched by this one.
