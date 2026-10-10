# ship_2026-09-21t-e — by-hand checks after installing

Four things in this round cannot be proved by a headless guard. Each says what to do, what
should happen, and what the failure looked like before the fix, so a "looks fine" is a real
observation rather than an absence.

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
