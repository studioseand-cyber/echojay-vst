# ship_2026-09-21t-f — by-hand checks after installing

This build carries all of 21t-e (packaged 26 Sep, never signed) plus three 21t-f items.
Seven things cannot be proved by a headless guard. Each says what to do, what should happen,
and what the failure looked like before the fix, so a "looks fine" is a real observation
rather than an absence.

## 1. The add-plugin picker over an INLINE hosted editor
The guard proves the picker CLOSES an inline editor and logs it. What it cannot prove is the
compositing: an inline hosted editor is a native view, and only a real window shows whether a
menu draws over it.

- On the Chain tab, open a third-party plugin **inline** in the rack — UnFairchild is the one
  this was found on (a JUCE built-in such as EchoJay EQ is pop-out only by design and will not
  reproduce it).
- With that editor open inline, press **"+"** to add a plugin.
- The picker must appear **on top**, and the inline editor must close as it opens.
- In the rolling log: `EJPicker: inline hosted editor closed - a heavyweight NSView composites
  over the picker`.
- **The defect:** the picker drew BEHIND the hosted plugin window — invisible, but taking the
  clicks. The 2 Sep fix covered the rack menu's inline case and the 22 Sep fix the picker's
  pop-out case; an inline editor under the picker fell between the two.

## 2. A rack click switches the chat, and the pill names the Link
- From the main chat, click a **Link's strip** (or pick its rack from the rack menu).
- The rack must come on screen **and** the chat must move to that channel. The composer pill
  must name the Link, not "This channel".
- A channel with no chat record yet is held as pending until the first send — that is the
  product's own rule, and the pill still names it.
- **The defect:** selecting a rack moved the view only, so a turn typed straight afterwards
  went to the mix bus while the rack on screen said otherwise.

## 3. A member click INSIDE a group keeps the group
- Select a group (e.g. Main vocals) so the chat is working on the group; its members'
  strips highlight.
- Click **one member's** strip.
- The view moves to that member's rack and the **group stays the chat target** — the pill still
  names the group, the members stay highlighted, and the next turn is still a group turn.
- Clicking a **non-member** leaves the group and switches the chat to that channel.

## 4. "harder" on a one-slot tuner chain
- Build a chain that is a tuner only (EchoJay Pitch), then say **"harder"** in that channel's chat.
- Nothing may touch the gain: no pre-gain is applied and **no calibration loop starts** — the
  loop starts only when the built or edited chain contains a compressor.
- The turn's body must carry `[CURRENT CHAIN]` for **that channel**, listing the tuner.
- **The defect:** a tuner-only chain came back with +4.4 dB of compose-time pre-gain, and the
  chain block went missing on Link chats when a parked Link had rewritten the sidecar.

## While testing
The rolling log is the record for all four: `EJPicker:`, `EJRackView:`/`EJTarget:`,
`EJGroupLevels:`, `EJClassify: body -- N turn(s) of chat`, `EJChat: CURRENT CHAIN injection
attached`. If something reads wrong, the line beside it usually says why.

## 5. SHORT90 on the group block (21t-f item 5)
- Let a Link's channel play for a minute or so, then select a group containing it and ask
  for something that makes a group turn (e.g. "level these").
- In the rolling log, the `EJGroupLevels:` line for that member must read
  `... SHORTMAX <n>, SHORT90 <n>, INT <n> ...` — SHORT90 immediately after SHORTMAX.
- SHORT90 is where the programme SITS (the p90 of the closed 3 s windows since the tally
  started); SHORTMAX is its loudest 3 s. SHORT90 should sit BELOW SHORTMAX, usually by a
  few LU on real material. It moves in 0.25 LU steps — it rides one byte of the frame,
  which is all the space there was.
- A Link on an older build prints `SHORT90 no reading`, never a borrowed figure.

## 6. Nothing writes to your own folders any more (21t-f item a)
- Nothing to do here, but if you ever wonder: `~/Documents/EchoJay` (Captures,
  References, Presets, the settings files) and `~/Library/EchoJay` are now off-limits to
  every test harness — they run write-sealed. The dev-mode chat-body dumps in
  `~/Documents/EchoJay` are yours again; harness runs had been evicting them.

## 7. A plugin removed mid-restore (21t-f item b)
- Worth one try since it used to be an abort: load a session with a chain in EchoJay, then
  REMOVE the plugin from the track immediately as the session opens (or close the session
  during load). Pro Tools should stay up. The log may say
  `EJState: deferred slot restore dropped - the plugin was removed before the message loop ran`.
