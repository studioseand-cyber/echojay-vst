# Show the real plugin: the live view without a copy
### His ruling of 26 Oct 2026. **Design only — nothing here is built.** Part 1 (the fallback: the copy leads) is being built in parallel and is §3.

---

## 1. Do the V2 and the Link run in the same process? Measured, per host.

### Logic Pro (AU) — YES. Measured on this Mac today.
Every EchoJay line in a session carries the pid of the process that wrote it, and in three separate
sessions there is exactly **one** pid carrying both the V2's own lines and the Links':

| session pid | lines only a V2 writes | lines only a Link writes |
|---|---|---|
| 23964 (26 Oct 07:21) | 12 | 891 |
| 24185 (26 Oct 07:22) | 17 | 846 |
| 25255 (25 Oct 20:38) | 37 | 2032 |

The system log goes further and puts them on **one thread**:

```
07:22:28.462 AUHostingServiceXPC_arrow[23964:2a67fda] (EchoJay V2)   EJPlace: "AUDelay" loaded at 48000 Hz
07:22:29.260 AUHostingServiceXPC_arrow[23964:2a67fda] (EchoJay Link) EJPlace: "AUDelay" loaded at 48000 Hz
```

Same pid, same thread id: Logic hosts both bundles in one `AUHostingServiceXPC_arrow` process, on one
message thread. (macOS 15.7, Logic as configured on this Mac. It is a host policy, not a law — see §5.)

### Pro Tools (AAX) — YES. Measured, 25 Oct 20:10–20:40.
```
Pro Tools[5151:278f53f] (EchoJay Link) ...   23,393 lines
Pro Tools[5151:27aa517] (EchoJay V2)   ...   and the V2's, same pid 5151
```
AAX loads into the host process itself: one pid, both bundles, no sandbox between them.

### Ableton Live (VST3) — NOT MEASURED. Live 10 and Live 11 Suite are installed here; EchoJay has
never been loaded in either, so this Mac holds no evidence and I am not going to assert a vendor's
policy from memory. **The probe that settles it in one minute:** load an EchoJay V2 on one track and
an EchoJay Link on another, then read one line — the V2 already logs `EJLinkShm: shared directory …
(pid N)` once per process, and the rack sidecar already carries `publisherPid`. Same number on both
sides = same process.

### Cubase / Nuendo (VST3) — NOT MEASURED, and not installable here: neither is on this Mac. Same
one-minute probe, on a machine that has it.

### And this is why the product must not hold a table of hosts
Process layout is a host **setting**, not a host fact: sandboxing can be switched on, a host can move
its audio engine into its own process between versions, and Rosetta or a bridge changes it again. So
the decision is made **at runtime, per Link, from one integer** — and that proof already exists in the
product and is already used by the agent's `open_editor where=embed` op:

```cpp
// Source/EJRemoteControl.h
inline bool embedAllowed (int linkPublisherPid, int ourPid) noexcept
{ return linkPublisherPid > 0 && ourPid > 0 && linkPublisherPid == ourPid; }
```

`publisherPid` is written into the rack sidecar by every Link (`LinkProcessor.cpp:837`). Unknown is
not permission: no pid, or a different pid, takes the fallback road and says so in a sentence.

---

## 2. "Show the real plugin" — the design where they share a process

The V2 and the Link are two `juce::AudioProcessor`s in one address space. Today they speak only
through files and shared memory, which is right across processes and absurd within one. Same-process
adds exactly one new thing: a **process-local directory of live EchoJay instances**.

```cpp
// A pointer is only ever handed out on the message thread, under this lock, after a uid match.
struct LocalInstances {
    static void add (const juce::String& uid, LinkProcessor*);     // the Link's constructor
    static void drop (const juce::String& uid);                    // the Link's destructor, FIRST act
    static LinkProcessor* find (const juce::String& uid);          // message thread only
};
```

### Who creates the editor, who owns it, on which thread
* The **Link's ChainHost owns the plugin instance**, as it does now. That never changes hands.
* The **editor is created by whoever will show it, exactly once, and is never moved between windows.**
  JUCE attaches a hosted AU's NSView to the window that holds the editor Component; moving it to
  another window means detaching and re-attaching that view — and that is precisely what killed Logic
  this morning (the nil-window transition; `docs/LIVE_VIEW_PATH_26oct.md` §4.1). So: no moves, ever.
* The V2 asks on the **message thread**: `Link::lendSlotEditor (slot, borrower)` → the Link calls
  `instance->createEditorIfNeeded()`, records the lease, returns a non-owning pointer and a token.
* The V2 `addAndMakeVisible`s it inside its pane; JUCE parents the NSView into the V2's window once.
* The V2 returns it with the token; **the Link destroys the editor** (only the owner deletes it).
* Nothing above touches the audio thread, takes a lease, a borrow or the rack lock — his 20 Oct
  ruling ("viewing does not engage") is kept by construction, because viewing now changes nothing.

### One editor per instance, and two surfaces that cannot disagree
`juce::AudioProcessor` holds one `activeEditor`; two parents is undefined behaviour. So the lease is
exclusive and **one owner answers the question**: `ChainHost::slotEditorHeldBy(slot)` → `none |
this Link's own window | a V2 (named)`.

* While the V2 holds it, the Link's own panel draws the slot with **"Open in EchoJay V2"** and will
  not open it; pressing it brings the V2's window forward.
* While the Link's own window holds it, the V2's card draws **"Open on the Link"** and does not open.
* Either side can ask the other to hand it over; the holder detaches first, then the new side opens.

### Removal, closing, deletion — the orders, which are the whole risk
The standing rule from two Softube crashes: *a hosted third-party AU is parked, never disposed
mid-session, and its editor is destroyed before it.* The lease makes the order explicit, and every
one of these runs on the message thread:

| event | order |
|---|---|
| the slot is removed | `removeSlot` → **revoke the lease** (the V2 detaches the child Component, synchronously) → destroy the editor → park the instance |
| the Link's own window closes | nothing: the V2 holds the lease and keeps showing it |
| the Link processor is destroyed (track deleted, plugin removed) | destructor's **first** act: `LocalInstances::drop` + revoke every lease → then the normal teardown |
| the V2 is destroyed while holding one | its destructor returns the lease before anything else |
| the host suspends or the session closes | unchanged: the instance is parked, the editor already gone |

A revoke that cannot reach its borrower (a borrower that has gone without returning) is a bug, not a
case to tolerate: the directory holds the borrower's pointer and the borrower de-registers in its own
destructor, so "the holder is gone" cannot be reached. The guard for it is a rig row that deletes the
Link's track while the V2 is showing its slot.

### Resizing
The real editor is an `AudioProcessorEditor`, so it says what size it wants and changes it when the
plugin does. The V2 keeps today's placement policy (native size, centred, top-pinned, clipped to the
card) but the mechanism becomes a **ComponentListener** on the editor — `componentMovedOrResized`
re-lays out once — instead of measuring an NSView on a 400 ms timer. Nothing polls; nothing touches
the view unless the plugin moved it. `isResizable()` editors are given the display area.

### The agent's hold
Unchanged in meaning, simpler in fact. While a run is **working** on that rack the read-only shield
goes over the real editor with the reason; after a stop the words and the knobs come back while the
rack stays held for "Undo all" (his 25 Oct ruling). What disappears is the whole question of a copy
and a real plugin disagreeing after a run: there is one instance, so there is nothing to reconcile.

### What the user gets
The real meters, the real gain reduction, the real knob under the hand, at the plugin's own frame
rate, with no mirror, no digest, no echo and no tap — because it *is* the plugin.

---

## 3. The fallback, where they do not share a process (and this is what is being built now)

**The copy leads while its view is open.** His words, as rules:

1. The copy owns its knobs. Its own editor moves them natively — we never write a value his hand is
   on, and we never write a value to make the picture catch up.
2. His moves travel **one way**: copy → ring → the Link's real plugin. Framed as one gesture, one
   undo step, exactly as today.
3. **Nothing** is written into the copy from the Link except a change that did not come from him:
   automation, the agent, or someone turning the knob on the Link's own window. The Link therefore has
   to say *whose* change it is, per parameter — a counter it bumps only when the change did not arrive
   through a `kSetParam` command — and the V2 applies a value only when that counter moved.
4. And **never** a parameter he touched in the last second, whatever the counter says.
5. **No timer touches the hosted view unless something is actually wrong**: the maintenance pass stops
   once the editor has settled, and re-runs on an event (our own layout, the editor resizing itself,
   the shield's reason changing), never on a clock.

An older Link that publishes no origin counters cannot support rule 3, so with one of those the copy
leads and **nothing** is written into it at all, and the card says so. That is the honest degradation;
guessing from values is the echo rule this replaces.

---

## 4. What this removes, and what it costs

### Removed by the real editor (§2)
| today | lines, roughly | fault families it has produced |
|---|---|---|
| FOLLOW layer 2: the values file, the schema digest, `lastApplied`, the held-by-gesture skip | ~400 | the knob pulled back; the knob frozen; the quarter-second jump |
| CONTROL C1–C3: the copy's listener, the echo test, the gesture framing, the refusal lines | ~500 | silent lost changes; refusals he could not count |
| the shadow audio tap: the ring, the 30 Hz publish, the frame counters, the staleness notes | ~600 | stale pictures; "is it following?" |
| the copy itself: a second instance, its seed state, its parking, `PluginCallWindow` | ~300 | a second instance crashing on load; two instances of a licensed plugin |
| NativeClip's stray reparent, for this road | ~40 | **this morning's Logic crash** |

### Kept either way
The clip container and its clipping; the read-only shield and its words; the placement policy; the
slot meters; the popout road for out-of-process editors.

### Cost, in rounds of the size we have been running
| | rounds | what each is |
|---|---|---|
| **§3 the fallback** (being built now) | 2 | (a) origin counters on the Link + one-way writes + the timer that stops, with the rig rows and the AUDelay crash fix; (b) whatever the first real-host run shows |
| **§2 the real editor** | 4–5 | (1) the process-local directory + the lease, with a rig row for every teardown order; (2) the V2's pane showing a lent editor; (3) the Link's panel and the V2's card agreeing about who holds it; (4) the removals above, one commit each with the guards they make redundant deleted in the same commit; (5) a full gate and a beta |

The order matters: the fallback is also what runs when the pid check says no, so none of it is
throwaway work, and §2 lands on top of a live view that already behaves.

---

## 5. What I would want you to know before approving §2
* The same-process answer is a host setting. Two hosts are proved here; Live and Cubase are unproved
  and the product will not assume — it asks the pid.
* An out-of-process AU UI (WaveShell and friends) keeps the popout road on both designs.
* The first round of §2 is the dangerous one: it is lifetime and ordering, where this plugin's worst
  crashes have come from. I would want its rig rows (remove the slot, delete the track, close the
  session, all while the V2 shows the editor) green before the V2 ever draws a lent editor.
