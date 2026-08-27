# Mixing the whole mix through the main plugin — requirements and decisions

Written 21 Aug 2026, from the live design conversation. **Not an implementation
spec.** It states the goal, the constraints already discovered, what is
decided, and what is still open. The implementation spec gets written against
it after a hosting survey.

Supersedes the per-slot model in `LINK_CHAIN_EDITING_HANDOVER.md` Stage 1 as
the *destination*; Stage 1's machinery is still the foundation (see §6).

---

## 1. The goal

A Link is an EchoJay instance on another track, streaming that channel's audio
back to the main and hosting its own rack.

**Select a Link's rack in the main → the whole rack comes across: plugins and
their settings. You edit any plugin in it, in place, hearing the real chain.
Deselect → the settings go back and the Link owns its rack again.**

The point is to mix the entire session from the main plugin.

## 2. Why whole-rack, and not one plugin at a time

Editing one slot in isolation cannot reproduce the chain, because chain order
cannot be reconstructed. If the Link bypasses slot 1 and streams audio that has
already passed slots 2 and 3, the local copy of slot 1 processes **last** — the
opposite of the real order. If it streams dry, slots 2 and 3 are missing
entirely. Either way you are mixing something that does not exist.

The whole-rack model is also **simpler** than the per-slot one: the Link
bypasses its entire rack once and streams dry; the main hosts the whole chain
in order. One bypass instead of N, one stream, one chain. Editing any plugin
becomes ordinary local editing — no per-slot lease, no pull-per-plugin.

## 3. Hard constraints, discovered and verified

These are facts about the codebase, not preferences.

**3a. Plugin instances are never freed.** `ChainHost` parks removed nodes in
`leakedNodeStore()`, a process-lifetime store, because plugins with leaked
repeating UI timers crash the shared AU hosting service when their memory is
freed (`ChainHost.cpp:755-790`). **The Link therefore cannot unload its rack
while the main borrows it.** Two live instances of every borrowed plugin is the
only available shape. Cost: memory and CPU for the duration. Accepted.

**3b. One `ChainHost` per processor.** `PluginProcessor.h:756` — one rack, one
`AudioProcessorGraph`. **The main must keep processing its own channel through
its own rack while hosting the borrowed one.** That is two independent chains
on two independent signal paths inside one plugin. The current design has one
of each. This is the largest structural item in the build.

**3c. State caps are two-tier, and the Link path uses the wrong tier.**
`ChainHost.h:885-904`: session tier 4 MB/slot, 16 MB total, protecting *a
document*; API tier 256 KB/slot, 1 MB total, protecting *a request* (the
platform's 4.5 MB body limit — a 4 MB slot base64s to ~5.4 MB). The Link pull
gates on `kApiStateMaxSlotBytes` (`LinkProcessor.cpp:652`) though it is a local
JSON file between two processes on one machine: no HTTP, no Postgres, nothing
shareable. The 1.1 MB sampler state that caused the session tier to be raised
on 17 Aug would be refused by a Link pull today. **Decide the Link's tier
deliberately.** The header's own instruction — do not align the two API and
session caps in either direction — still stands; this is a third consumer, not
an alignment.

**3d. The editor is not destroyed on a Logic Link-window switch** (measured:
CTOR 0, DTOR 0 across 20). Closing the plugin window does destroy it. Anything
holding ownership must key off the right one of those.

**3e. `baseSlots` is names-in-order**, so a swap of two identically-named slots
passes the staleness guard. KNOWN LATENT DEFECT, deliberately not fixed in the
rack-lock change (brief §2c): the apply-time comparison is
`ChainHost::applyChainEdits` pre-flight guard 2 — `ChainHost.cpp:1588` (count)
and `ChainHost.cpp:1603` (`namesMatchLoose(baseSlots[i], slots_[i].desc.name)`
per index) — which cannot distinguish two slots whose names match after a
reorder. The whole-rack spec inherits this as a stated problem: any
identity-carrying replacement (slot uid/fp riding `baseSlots`, per the sidecar
identity fields) should close it. Note the rack lock REDUCES its exposure —
while a main holds the lock the Link cannot reorder underneath an in-flight
op — but the mismatch-window between two mains' FCFS handoffs remains.

**3f. The rack selection is soft while its channel has no chat.** The Chain
tab's rack view, the rack lock and the chat channel are ONE selection
(`effectiveChannelUid`, PluginEditor.cpp) by design — but for a channel with no
chat the selection is held as `pendingChannelUid`, which applies only while
`currentChatId` is empty. Any chat activation from any of the chat system's
writers (sidebar click, deep link, editor-recreate restore, edit-card target
switch) silently snaps the selection — and with it the rack view AND the rack
lock — to that chat's channel. KNOWN LATENT HOLE, deliberately not fixed with
the rack lock (21 Aug 2026). Whole-rack borrow leans on this selection being
stable while a rack is held, so the borrow spec inherits it as a stated
problem: either the borrow pins its own uid for the borrow's duration, or the
pending selection gets a harder home.

## 4. Decided

**Ownership is a UI lock, never an audio change.** While a main holds a Link's
rack, the Link's own rack UI is read-only and greyed, with an overlay naming
the owner and how to release it. The `Active` toggle is never touched.

**The lock is held by a live editor actively showing that rack** — processor
renewed, editor gated, deleted on clear. Selection alone would strand a lock
with no visible owner, because the selection is processor-held and survives
editor recreation.

**Reverse contention: the Link keeps it, by recency not by window state.** If
the Link made a local rack edit in the last ~10s the main waits and acquires
when quiet. Link windows sit open idle all day, so window-open is not a usable
test.

**Two mains: first come, first served**, second sees the same overlay naming
the first, auto-acquiring on release or expiry.

**Read-only covers structure and mix writes** — add, remove, move, bypass, slot
wet, master wet. ~~Plugin interiors stay live: selection, scrolling, meters, and
opening a hosted editor.~~ Locking parameter writes would be a claim the code
cannot keep.

**AMENDED 2 Sep 2026 — the lock now covers hosted editors too.** This
reverses the struck sentence above: on lock acquire the Link closes any
open hosted editor and refuses to open new ones while locked, with the
reason stated. The reason for the reversal: the session's write-back at
deselect replaces plugin state wholesale, so interior edits made on the
Link during a lock are CLOBBERED when the session ends — an editor that
stays open is an invitation to lose work. Selection, scrolling and
meters stay live; parameter-write locking remains a claim the code
cannot keep, which is exactly why the editor must close instead. No
dimming overlay: see the standing hazard below.

**STANDING HAZARD — hosted NSViews composite over everything JUCE
draws.** A hosted plugin editor is a native NSView; it ignores JUCE
z-order AND JUCE visibility of overlapping lightweight components.
Three defects in one week came from forgetting this: the chain-build
auto-open painting a plugin box over other tabs, the rack menu drawing
behind the plugin pane, and the review overlay's earlier rule ("NSViews
composite over lightweight components, so they must be HIDDEN, not
out-z-ordered"). The rule: anything that must appear over a hosted
editor requires the editor CLOSED or HIDDEN first — never an overlay,
never z-order.

**STANDING HAZARD 2 — some hosted native views report success and
render nothing.** WaveShell's embedded AU view returned a valid frame
(481x614), the settle poll "settled", and the pane stayed blank: a
view that measures fine and draws nothing defeats a size poll BY
CONSTRUCTION. Detection for this class must be by IDENTITY (catalogue
manufacturer/shell, decided before any embed attempt —
ChainHost::floatsByIdentity), never by measurement. Do not fold the
identity rule into the poll as redundant; the poll cannot see this
failure and never will.

**Licensing is not a concern.** iLok is per-machine; multi-instance is normal.
Hardware-DSP plugins are the one real exception and are not worth designing
around.

## 5. Open, with recommendations

**5a. Commit model — the most dangerous open item.** "Push back on deselect" is
an implicit commit: a crash, a quit or a mis-click loses the session's edits
with no moment where the user said *write this*. Recommend either an explicit
Apply, or continuous write-back so there is nothing to lose. Stage 1's
*Apply and Release* was explicit for this reason.

**5b. The second hosting context.** A second `ChainHost` for the borrowed rack,
or one graph carrying two parallel chains with separate ins and outs. Needs the
survey in §7.

**5c. Withheld slots at rack scale.** Two of five slots refusing their chunk
means the chain being mixed is not the Link's sound. Per-slot notes exist; at
rack scale this must be loud and up front, not a list to scroll.

**5d. What the Link's own window shows while locked.** Greyed with the overlay,
nothing, or a read-only mirror of the main. Decides how much stays in sync.

**5e. Total-bytes behaviour** when a whole rack exceeds whatever cap §3c
settles on: refuse, or partial with a named list.

## 6. What survives from Stage 1

The per-slot edit path is likely superseded, but three pieces get *more*
important at rack scale and should not be retested as though they were:

- **The state codec** (`LinkShm::stateToB64` / `stateFromB64`, one author) —
  the same transfer, N times over.
- **Identity matching** (format/uid/version, apply-time re-check, the deadman)
  — a bad match now poisons a whole chain rather than one slot.
- **The lease machinery** (heartbeat, expiry, dead-id memory, processor-held) —
  already the basis of the rack lock.

Do not spend a day proving the per-slot guards if the per-slot flow is being
replaced. Prove the shared machinery.

## 7. Next, in order

1. Finish the rack lock (in build).
2. **Survey the hosting architecture** against §3b and §5b: what it takes for
   one plugin to host two independent chains on two signal paths, and what that
   does to `prepareToPlay`, latency reporting and the graph.
3. Write the implementation spec against this document.
4. Build, with the solo path first — it already works and proves the transfer.

## 8. Parked

**In-context (non-solo) monitoring and delay compensation.** Blocked on a spec
answer, not on code: what happens when the alignment stamps are present but
drift mid-edit. That failure is silent and sounds like a mix decision rather
than a fault, so it must be answered before it is built. Solo works today and
proves everything else.
