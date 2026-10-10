# ship_2026-09-21t-g — by-hand checks after installing

On top of the installed 21t-f. Two things this build does that nothing headless can check: the calibration loop
runs **passively** on an ordinary build, and the teardown crash that had been killing the guard suite is gone.

## 1. A compressor on a vocal Link, with the song playing — NOTHING is asked of you
This is the whole point of the round: by the time you ask for a compressor, EchoJay has usually been keeping
figures for minutes, so it should set the thing and get out of the way.

- Play the song. On a vocal Link's chat, ask for a compressor (or ask for the chain to hit harder on a chain that
  has one).
- **No card, no "Listening…", no "play the loudest part"**, and nothing asking you to press Listen. If you see any
  of those words on a build you did not ask to calibrate, that is the defect.
- In the rolling log, the loop names its mode and its knob on every window:
  `EJCalib: "<plugin>" window N gr=<n> <knob>=<value> mode=passive state=listening`
  (a drive pass prints `pre=`/`post=` instead of a knob name).
- The value moves in **1 dB steps**, at most six of them, and stops when two consecutive 3 s windows sit in the
  band. A threshold pass moves the plugin's own threshold control and leaves the drive where the staging put it.
- **One closing line, and only if something moved:** `Adjusted the <plugin> <knob> to N dB.` — no question
  attached. If the compressor was already in the band, **nothing is said at all**. That silence is correct.

## 2. The turn carries the channel's own numbers
- On that same turn, the log shows `EJTrackLevels: [TRACK LEVELS - "<channel>" (id <uid>)] trim …, MOM …, SHORT …,
  SHORTMAX …, SHORT90 …, INT …, PEAK …, PSR …, HEARD …`.
- `HEARD` should be the real time the Link has been hearing that channel. If it reads under 30 s, the server sends
  a **listen** block instead and the old card behaviour is correct for that turn.

## 3. A group turn still carries the group block, not this one
- Select a group and ask for something. The log must show `[GROUP LEVELS …]` and **no** `[TRACK LEVELS]`.

## 4. The teardown crash (for confidence, not a test)
- Open and close a session a few times with a chain loaded, and remove a plugin mid-load once. Pro Tools should
  stay up. The guard suite's scribble leg — which is the same teardown path — is now 6/6 clean where it used to
  fail about half the time.

## 5. Nothing about this build touches your folders
`~/Documents/EchoJay` and `~/Library/EchoJay` are off-limits to every harness (21t-f), and a Link registry slot now
refuses a uid it cannot hold whole rather than truncating it in silence.
