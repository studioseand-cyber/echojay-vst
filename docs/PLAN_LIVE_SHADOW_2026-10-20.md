# Plan: the muted shadow — a LIVE plugin in the V2's view of a Link slot

**Sean's design, 20 Oct 2026.** The Link stays the only real processor and what is heard never
changes. The Link streams the shadowed slot's **input** to the V2; the V2's hosted copy of that slot
processes it in parallel with its output discarded, so the copy's own meters and GR move like the real
one's. The Link is the one parameter authority: the copy follows it continuously, a knob turned on the
copy is sent to the Link as an edit over the wire, and the copy then follows the Link back. The copy
never writes its own state anywhere. Only the slot in view is shadowed, and it stops when he leaves
it. No borrow session, no floating windows.

**Status: plan only. Nothing built.**

---

## 0. What already exists (this is most of it)

I went looking before estimating, and four of the five pieces are already in the tree:

| Piece | Where | State |
|---|---|---|
| Audio transport Link → V2 | `LinkShm.h`: per-Link SPSC ring `audio_<uid>.bin`, 65536 frames × 2 ch float, with `sampleRate`, a host-position seqlock stamp and seek-to-cushion | **Production.** Used by capture, solo and the edit audition |
| A hosted copy of ONE slot, fed from that ring, output discarded | `EchoJayProcessor::editBegin` + `editInst_->processBlock(editBuf_, noMidi)` in `processBlock` | **Production, and already discarding.** Nobody has listened to the result since 11 Oct |
| V2 → Link parameter channel | `EJCmdRing.h`: `kSetParam`, "one parameter on one slot's device, in the device's own units (HUMAN GESTURE only)", `paramId` indexes the slot's published schema-id list, `structureRevision` in `flags`, newest-frame-wins | **Production.** "Values arrive within one buffer", with a file-channel fallback |
| Link → V2 value channel | `RackSidecarSlot.settings` + `landedValues`; published when `chainRevision` or `getHostedChangeEpoch` moves | **Production.** A real hosted plugin bumps the epoch when a parameter moves |
| The shadowed slot's **input** audio | — | **The one genuinely new piece** |

The ring's own header calls this "stage 0 of remote editing" and names what is still owed: *"stage 1's
alignment FIFO will [seek]"*, and *"readIdx is one cursor, so a seeking consumer and a capture on the
same Link are mutually exclusive. That exclusion is stage 1's problem to enforce."*

**And the reason the existing audition was disconnected is the reason Sean's design is right.** The
11 Oct note in `processBlock` says it plainly: *"The ring taps POST-RACK, and since stage 1 the Link's
own rack is no longer bypassed while addressed - so this consumed a channel that had already been
processed and processed it AGAIN. Anything that MEASURED on that copy was measuring double processing;
the compressor calibration did, and it was invisible precisely because nobody could hear it."*

Taking the **slot's input** instead of the rack's output is exactly what makes a copy's meters mean
something. The feature is not new machinery; it is the existing machinery aimed at the right tap.

---

## 1. Audio transport

**A second ring per Link, not a second consumer of the existing one.** `readIdx` is a single cursor,
so capture, solo and the audition are already mutually exclusive — adding the shadow as a fourth
consumer of `audio_<uid>.bin` would mean the shadow silently stealing frames from a capture, or an
exclusion rule that switches the feature off whenever he bounces. So: `shadow_<uid>.bin`, identical
layout, written by the Link at the **input of the slot the V2 asked for** and at no other time. One
ring per Link, not per slot, because only the slot in view is shadowed.

- **Which slot** rides the existing `onEditorRequest`-shaped road: a V2 → Link request naming the slot
  (or −1 for "stop"). The Link writes nothing while that is −1, so the cost of the feature when it is
  not on screen is zero — not "small", zero.
- **Latency.** Cushion 1024 frames, reseek trip 8192 (`kEditCushionFrames` / `kEditReseekTrip`). At
  48 kHz that is **21 ms of cushion and a 171 ms outer bound**, plus up to one block if the host runs
  the V2 before the Link in its graph (which is not guaranteed in either host). So the shadow's meters
  lag the real ones by **about 25 ms at 48 k / 512**. For level and GR movement that is invisible. For
  a transient-accurate GR needle it is not zero, and the card should never claim the two are the same
  instant.
- **Sample rate.** Identical by construction — one host, one engine — so the design **compares and
  refuses** rather than resampling. A mismatch means something is wrong, not something to paper over.
- **Block size.** The ring decouples them: the copy is prepared at the V2's rate and block, and asked
  for `buffer.getNumSamples()` frames per block whatever the Link's block is. The existing guard stays:
  a host block larger than `editBuf_` (8192 frames) skips the block rather than allocating on the
  audio thread.
- **Underrun is silence, never stale samples** — the existing rule, kept.
- **Drops.** A partial produce breaks the stamp's linear mapping until the next block re-pairs it. The
  shadow does not need sample-accurate alignment (nothing is summed, nothing is heard), so it reads the
  stamp only to decide whether it has fallen behind and should re-seek.

## 2. CPU cost

One extra instance of **one** plugin, running at host rate, only while that slot is on screen. That is
honestly "+1 slot's worth of DSP" — nothing for an EQ or a compressor, and a real cost for an
oversampled limiter or a convolution reverb, which can be the most expensive plugin in the session.
The ring copy itself is two float stores per frame per channel: negligible.

So the design needs a **cost bound with a visible ending**: time the copy's `processBlock`, and if it
takes more than a set share of the block period over a short run, stop shadowing, fall back to the
static picture, and say which plugin and what it cost. A feature that can silently halve his track
count is worse than one that admits it cannot shadow a particular plugin.

## 3. The parameter follow, and how it avoids loops and fights

**The Link is the authority; the copy is a renderer with a keyboard.** Both directions already exist,
so the work is the discipline between them, not the transport:

- **Link → copy (continuous).** The copy applies the slot's published values. Identity comes from the
  slot's **schema-id list** the value ring already indexes, so a value is matched by id, never by
  position — the lesson from every index-as-identity defect this month. The `structureRevision` the
  ring carries in `flags` is checked on the way in: a value for a structure the copy no longer has is
  dropped, not guessed at.
- **Copy → Link (a gesture).** A knob turned on the copy sends `kSetParam` — which the ring already
  defines as **human gesture only** — and then the copy waits to be told. It does not apply its own
  move locally and it does not keep its own value.
- **No loop, by construction:** the copy writes only on a gesture, and follows on everything else.
  The echo (the Link publishing back the value we just sent) is not fought with a timer; it is
  **absorbed**, because the copy's job is to follow and the echo is what it was waiting for.
- **No fight:** one writer per control is the standing rule (21t-m). The copy never writes the slot's
  own state anywhere, which is precisely what made the old mirror go stale: *the mirror never saw a
  wire chain-cmd, so it aged.* A renderer that holds no state cannot go stale.
- **A gesture mid-flight** (he keeps turning while the Link is answering) coalesces the way the ring
  already coalesces: newest frame per `(kind, slot, paramId)` wins.

## 4. Licence-checked plugins and a second instance

This is the biggest product risk, and it is not solvable in general — only refusable well. Three
failure modes, all real on this Mac:

1. **Instance-limited licences** refuse the second instance outright.
2. **A wrapper that prompts.** A PACE/iLok dialog is exactly the thing that must never happen, and on
   this Mac a full AU scan has crashed the machine.
3. **Single-instance-by-design plugins**, which may fail, or may misbehave more quietly.

So: the copy is created **on the message thread, with a time bound**, never at a transport edge; a
failure or a timeout falls back to the static picture with one plain line naming the plugin; and the
outcome is recorded in the per-plugin record that already exists (`plugin_disabled.json` /
`known_good.json`) so it is never retried blindly. **The shadow is never the only road to seeing a
slot** — the static picture stays, and the shadow is an upgrade on top of it.

## 5. Logic AU vs Pro Tools AAX

- **Logic.** Both are AUv2, in-process, sharing `TMPDIR` — which is what `LinkShm`'s directory
  resolution was built for. No new ground.
- **Pro Tools.** The pair are AAX in the Pro Tools engine. The shm directory falls back to
  `~/Library/Application Support/EchoJay/link/`, which both reach, so the transport holds. Three
  differences that matter: hosting a guest AU/VST3 inside an AAX plugin is what `ChainHost` already
  does, so the second instance is not new ground either; Pro Tools is stricter about work and dialogs
  near the audio thread, which is another reason instantiation is message-thread-with-a-bound; and the
  V2/Link order in its graph is less predictable, so the cushion earns more of its keep there.
- **`tools/quick.sh` does not build AAX** (four targets: AU + VST3 for both). A Pro Tools proof needs a
  full-gate build, so Pro Tools cannot be a QUICK round.

## 6. How long

| Stage | Work | Estimate |
|---|---|---|
| A | The slot-input tap and `shadow_<uid>.bin`; the tap selector; zero cost when off | ~1 day |
| B | The V2 side: `shadowBegin/End` reusing `editBegin`'s instance machinery **without the lease**; meters and GR read off the copy | ~1 day |
| C | The follow both ways: by schema id, structure-revision checked, gesture-only upstream, echo absorbed | ~1.5 days |
| D | The refusals and their guards: licence/second-instance, CPU bound, SR mismatch, ring exclusion, leaving the slot | ~1 day |

**~4.5 days of work; a week with gate and rig time.** It is not a QUICK round: it touches the audio
thread in both binaries, so it needs a full gate, and Pro Tools needs an AAX build.

## 7. The cheaper thing, said once

`ChainHost::onEditorRequest(slot, open, embed)` and `editorPeerPid` (a same-process proof) are already
in the tree, and in Logic both plugins are in-process. **Embedding the Link's own editor view in the
V2's window** would be live by construction: no second instance, no licence risk, no audio transport,
no parameter follow, nothing to go stale — because it is the real plugin, not a copy of it. It is not
the "bring it forward" idea Sean rejected; the view is parented into the V2's window, not raised
somewhere else.

If what he wants is "the plugin shown for a Link slot is live and real", that is a fraction of the
work above. If he wants the V2 to own the surface — its own look, its own layout, no guest NSView —
then the shadow is the design, and section 0 says most of it is already built.

**Sean's call. Nothing starts until he rules.**

---

## 8. The embed option, with its risks (Sean asked, 20 Oct)

I raised embedding in section 7 as "a fraction of the work". Having read what is actually there, **I
was too optimistic and I'm correcting it.** The groundwork exists, but what it gates today is not
what section 7 implied.

**What `embed` means today.** `ChainHost` honours `open_editor … where="embed"` by asking the LINK to
show that slot **inline in the Link's own window** (`LinkEditor::timerCallback` → `selectSlot`, and
`openPopoutForSelected` only when NOT embedded). Nothing re-parents anything into the V2. The
same-process proof (`echojay::embedAllowed`, `editorPeerPid` vs `getpid()`) is real and already
enforced, and `EJRemoteControl.h` states the principle this plan has to live with:

> *"AN EDITOR BELONGS TO THE INSTANCE THAT OWNS IT. The V2 cannot show a window for a plugin living
> in another process … `embed` IS THE NICER ONE AND IT IS NOT ALWAYS SAFE. In Logic today both
> plugins do share a process - every one of the six AUHostingServiceXPC crash reports from 8 October
> lists EchoJay V2 and EchoJay Link in one process's usedImages. That is a fact about Logic's AU view
> hosting, NOT a guarantee."*

**Pro Tools AAX: no, in practice.** The proof is a runtime pid comparison, so in AAX it simply fails
and falls back to a floating window — by design, never inferred from the host's name. The header
names AAX explicitly as having "its own rules". So an embedded rack view would be **Logic-only**, and
in Pro Tools the rack view would stay exactly as static as it is now. For a feature whose whole
purpose is "the plugin shown should be LIVE", that is a serious limitation.

**The Link's window open, closed, or the Link removed.** A view has ONE parent, and this is where the
risk concentrates:

- **Window already open on that slot:** embedding the same editor into the V2 must first remove it
  from the Link's window, so opening the rack view would visibly *steal* the editor out of the Link.
  Two owners of one view is the defect class, not a detail.
- **The Link's window closes:** the Link's chain panel owns that editor and would delete it, while
  the V2 still has it as a child — a dangling child component, i.e. a crash. Avoiding that needs a
  shared ownership contract between two plugin instances: the Link must not delete a view the V2
  holds.
- **The Link is removed** (deleted, or unloaded by the host): its processor and editor are destroyed
  and the V2's window is left pointing at freed memory. That is a use-after-free, the same family as
  the 18 Sep Pro Tools crash and the latency-rebuild UAF that still owes an ASan run.
- **A suspended or offline track:** a view parented into the V2 drawing for a processor the host is
  not running.

**Re-parenting a host-owned view: yes, a real crash risk.** The guest creates its own NSView and is
entitled to assume its window does not change under it. Plugins cache the window, the screen scale,
or install window-level event monitors at attach time; a parent change breaks those, and the common
failure modes are resize storms, lost mouse capture and blank views, with a minority crashing. On
this Mac specifically, both plugins already share one AU hosting process that has six crash reports
from 8 Oct and a known UAD teardown SIGSEGV at exit — adding cross-instance view ownership there adds
a new way for one plugin's teardown to take the other's window down.

**And the obvious way round it does not exist.** The safe version of "embed" would be for the V2 to
ask the Link for a SECOND editor on the same processor, so no view ever moves. JUCE's contract is one
active editor per processor (`getActiveEditor`), and many plugins do not tolerate two. So a safe
embed essentially requires the view to move, and moving is the risky part.

**How long.** A **1-day spike first**, against his real plugins, to find out how many survive
re-parenting at all — that number decides whether the feature is viable, and no estimate before it
is worth much. Then ~2-3 days for the ownership contract (the Link never deletes a view the V2 holds;
the V2 drops it the instant the sidecar's pid or the slot identity moves; a watchdog for the
removed-Link case) plus the float fallback for every host that fails the proof. Its guards can only
be driven in-host, so the rig carries it. **~3-4 days, Logic-only, with a materially higher crash
risk than the shadow and a hard dependency on each plugin's tolerance.**

**So the comparison, corrected:**

| | Shadow | Embed |
|---|---|---|
| Works in Pro Tools AAX | Yes | No (falls back to floating) |
| Crash risk | Ordinary (a second instance, refusable) | Higher: shared view ownership, re-parenting, use-after-free on removal |
| Per-plugin tolerance needed | Second instance / licence | Re-parenting, which fewer plugins tolerate |
| Cost | +1 slot of DSP while on screen, zero off | Near zero |
| Effort | ~4.5 days | ~3-4 days, after a 1-day viability spike |

**The shadow is the safer design**, even though it is the bigger-sounding one, and it is the only one
of the two that can work in both his hosts.
