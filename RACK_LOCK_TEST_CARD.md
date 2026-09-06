# Rack lock — exact steps

Installed DEV build: AU `E22B2D88-309D-3EA5-B336-033FB29CFAB0`,
VST3 `7E6C5057-6B45-3CD5-B349-19E977513728`.
Lock file lives at
`~/Library/Application Support/EchoJay/link/racklock-<uid>.json`.
Timings that decide every wait below: **renew 1s, expire 3s, recency 10s.**

---

## SETUP — do this once, in this order

**S0. Both binaries, verified by content.** This build ships TWO plugins —
`EchoJay V2` and `EchoJay Link` — and the lock is half in each. Installing only
one produces a main that locks perfectly against a Link that ignores it.

```
strings ~/Library/Audio/Plug-Ins/Components/EchoJay\ V2.component/Contents/MacOS/EchoJay\ V2 | grep -c EJRackLock
strings ~/Library/Audio/Plug-Ins/Components/EchoJay\ Link.component/Contents/MacOS/EchoJay\ Link | grep -c EJRackLock
```

Both must be non-zero (Link side: 6). A zero on either and nothing below is
readable. Installed 21 Aug: Link AU `0B906A36-4B0D-3648-BFF1-063480F78B9E`,
Link VST3 `D776F9B4-1E68-3AC3-9306-0B103AE0AB63`.

**S1.** Quit Logic. Then in Terminal:

```
killall AUHostingService AUHostingServiceXPC_arrow
```

(Errors saying "no matching processes" are fine — it means they were already
down.)

**S2.** Open a second Terminal window and leave it running:

```
log stream --predicate 'eventMessage CONTAINS "EJRackLock"' --style compact
```

Every step below is read from this window. **The log line is the witness** — a
guard that returned early with no refusal looks identical, in the UI, to one
that refused.

**S3.** Open a third Terminal window for the lock file:

```
watch -n1 'ls -l ~/Library/Application\ Support/EchoJay/link/racklock-*.json 2>/dev/null || echo NO LOCK FILE'
```

If `watch` isn't installed, just re-run the `ls` by hand when a step asks.

**S4.** Open Logic. One track with the main EchoJay, at least one other track
with an EchoJay acting as a Link. For step 7 you need a **second** main as well
— a third track with EchoJay — but don't add it until you get there.

**S5. Confirm you are on this binary.** In the main, select the Link's rack by
name (the rack selector you used in the earlier Link tests). Within ~1s, the
watch window must show a `racklock-<uid>.json` appear. Then:

```
cat ~/Library/Application\ Support/EchoJay/link/racklock-*.json
```

`owner` must read your main's track name. **If no file appears, stop here** —
you're on an older binary and nothing below means anything.

Now deselect the rack. The file must vanish. That's your baseline.

---

## THE SEVEN

Stop at the first genuine failure. Bring the observation and the log lines —
not a theory.

### 1. It looks locked

Select the Link's rack in the main. Bring the **Link's** own window forward.

**Expect:** its rack blocks dimmed, and the banner:

> Selected in the rack on "*&lt;your main's track&gt;*" - deselect there to edit here.

**Log:** `EJRackLock: acquired uid=…` (main) and `EJRackLock: locked by "…"`
(Link).

Screenshot the Link window. Leave the rack selected — steps 2 and 3 need it
still locked.

### 2. Six controls dead

**Corrected 21 Aug — the original version of this step was wrong.** It expected
a `EJRackLock: refused <op>` log line per attempt. In fact all six writes are
stopped in the UI *before* they reach the processor guard: the bypass, remove
and move buttons are `setEnabled(false)`, the slot wet knob is hidden, the add
block is disabled and the master knob stops intercepting clicks
(`LinkEditor.h:645-663`). The guard is a backstop, so **a correctly locked rack
logs nothing when you click at it.** Silence here is the pass, not the failure.

Still on the locked Link, try each **one at a time**:

| # | Do on the Link | Pass looks like |
|---|---|---|
| 2.1 | Add a plugin to the rack | "+" dead, tooltip names the owner |
| 2.2 | Remove a rack slot | remove button dead, tooltip |
| 2.3 | Move a slot up or down | prev/next dead, tooltip |
| 2.4 | Click a slot's bypass | bypass dead, tooltip |
| 2.5 | Reach for a slot's wet | knob is **not shown** |
| 2.6 | Drag the master wet knob | knob ignores the mouse, dimmed |

Blocks sit at 55% alpha throughout. **Nothing in the rack may change**, on
either window.

A control that still works is the failure this step exists to find. If one
moves and the rack changes, note *which*, and whether the main's view followed
it — that tells us whether the write reached the rack or only the UI.

If a control moves and the log **does** show `EJRackLock: refused …`, that's
the backstop catching a gesture the UI failed to stop: not a lock failure, but
a real gap worth reporting.

### 3. Interiors must stay live

Still locked. On the Link, open a hosted plugin's editor from a rack slot and
move a control. Scroll the rack. Select a different slot.

**Expect: all of it works.** Blocked here is a **fail** — the guard is too
wide. Read it that way, don't treat it as extra safety.

### 4. Window switch holds; window close releases

**4a.** With the rack still locked, switch Logic to another plugin window and
back to the main. Check the watch window: the lock file must **still be there**,
and its timestamp must keep moving (it renews every 1s).

**4b.** Now close the main's plugin window entirely. Within ~3s the file must
disappear.

**4c.** On the Link, **add a plugin.** It must succeed, with no refusal line.

4c is the actual test. The banner disappearing proves the Link redrew; the add
proves it can write.

### 5. Recency, then auto-acquire

Reopen the main and make sure the rack is **deselected**.

**5a.** On the Link, make any local rack edit — a bypass click will do.

**5b.** Immediately (inside 10s) select that rack in the main.

**Expect in the main:**

> *&lt;rack name&gt;* was just edited in its own window - this clears on its own in a few seconds.

**Log:** `EJRackLock: waiting - the Link was edited moments ago`.

**5c.** Now do nothing. Watch. Within ~10s of your Link edit it must acquire —
`EJRackLock: acquired uid=…`, the message clears, **with no further click from
you.** If you have to click to make it take, that's a fail.

### 6. Owner gone

**6a.** With the rack locked from the main, force-quit Logic (⌥⌘Esc → Force
Quit) — or force-quit the AU host process if the Link is in a separate session.

**6b.** Watch the lock file. Within ~3s it must go, or go stale.

**6c.** Reopen and **add a plugin on the Link.** It must succeed.

Again: prove it by the add, not by the banner.

### 7. Two mains

**7a.** Add EchoJay to a third track — this is main #2.

**7b.** Main #1 selects the Link's rack. Confirm it holds (step 1's log line).

**7c.** Main #2 selects the same rack.

**Expect in main #2:**

> This rack is locked by *&lt;main #1's name&gt;* - it frees up when that window deselects it.

**Log:** `EJRackLock: held by "…"`.

**7d.** Main #1 deselects. Main #2 must acquire **on its own**, no click.

---

## The one judgement call

Steps 5 and 7 must read as **different situations**, not two phrasings of
"locked". 5 clears by waiting; 7 clears only when someone else lets go. If you
look at either message and can't tell which kind you're in, amendment 2b isn't
satisfied even though both strings exist — say so, it's a real finding.

## Clean up after

Deselect everywhere, then confirm no `racklock-*.json` files are left behind:

```
ls -l ~/Library/Application\ Support/EchoJay/link/racklock-*.json
```

A leftover file after everything is deselected and closed is its own bug.
