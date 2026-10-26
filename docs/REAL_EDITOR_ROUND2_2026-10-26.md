# The real editor, round 2: what works, what blocks it, and the one question left
### 26 Oct 2026, written at 21:05 after four crash investigations. **Nothing of this is installed.**

Sean is on **V2 AU `9B29E0FE` / Link AU `F95EC755`** (round 1 plus the transport-stopped fix), which he
tested green. Round 2's code is committed behind his switch, and the switch is **off by default**, so a
build carrying it behaves exactly as the one he is using.

---

## 1. What round 2 proved, and it is not small

| row | what it measures | result |
|---|---|---|
| RE8 | with the switch ON, the pane shows the **Link's own editor** and **no copy is started at all** | PASS |
| RE9 | five seconds hands-off, judged in pixels and frame | PASS - *frame unchanged, pixels unchanged* |
| RE0-RE4, RE7 | the directory, the lease, exclusivity, the default road | PASS |
| LV1-LV11 | the whole **copy road**, including his two new acceptance checks | PASS, 0 skipped |

His wobble fault (fault 3 of 26 Oct) is **gone by construction** on the real road: there is no mirror, no
timer and no copy, and the picture does not move when nobody touches it.

The copy road is in better shape than it was this morning, and that part is installed and tested:
LV9 (a change reaches the Link and stays), LV10/LV10b (the same with the transport stopped, and the
hand-over under load), LV11 (the V2's own output unaffected by the live view).

## 2. The four crashes, each with its identity

Every one of them was at the same door: **closing and re-opening a lent editor.**

1. **`-[NSBox priv_eventListener:event:value:]: unrecognized selector`**, inside
   `AUEventListener::MessagesAreAvailable` <- `CAEventReceiver::Impl::timerFired`. A destroyed AU view's
   event listener, delivering to freed memory on the next parameter write.
   *Fix tried:* keep one editor per instance, destroy only on teardown.
2. **`EXC_BREAKPOINT` in `-[NSApplication _crashOnException:]`** during a CoreAnimation flush, with
   `*** Assertion failure in AUDelayCustomView.mm:176` under `_NSViewDrawRect`. Apple's own view,
   asserting while drawing.
   *Cause found:* `dynamic_cast error 2: type_info has hidden visibility or is defined in more than one
   translation unit` - the V2 identified the plugin's NSView with `dynamic_cast`, which **cannot work
   across the two bundles**, so the container held a view that did not belong to the editor on screen.
   *Fix:* `EditorLender::lenderGuestViewForSlot` - the lender answers, in its own image, and if it
   cannot the V2 **does not guess**: it returns the lease and the copy road takes it, with the reason.
3. **The stranded view.** With the editor kept across a returned lease, the plugin's view stayed inside
   the V2's container with nobody responsible for it (`NativeClip2_detachEmptyContainer` refused to
   remove a container that still held a live view, and said so). The next opens stacked a second guest
   view in the same container.
   *Fix:* back to the design's teardown table - a returned lease destroys the editor, and a borrower
   that still wants it **hides** it rather than returning it (his option 3).
4. **`SIGSEGV`, same door.** The log's own account:

   ```
   20:59:56.433  EJLease: made the real editor for slot 1 of ... (uid b85ffc55c8)
   20:59:56.589  EJLease: destroyed the real editor for slot 1        (156 ms later)
   20:59:57.821  EJLease: made the real editor for slot 1             (again)
   20:59:57.821  NativeClip2: putting AUDelayCustomView ... into the container
                 -> dead
   ```

## 3. The one question left, and why it is a design question

Switching the rack in view is an ordinary thing to do, and with "a returned lease destroys the editor"
it means **create, destroy, create** of a real AU editor on one long-lived instance within two seconds.
Apple's AUDelay view does not survive that. The copy road never met it because every copy is a fresh
instance that is thrown away; a Link's instance lives for the session.

So the two halves we need are in tension:

* **destroy on hand-back** keeps the view's lifetime simple, and kills the plugin's own view faster
  than it tolerates;
* **keep one editor per instance** matches the design's words ("created exactly once") and the plugin's
  tolerance, but then the view must not be stranded in the borrower's window when the borrower lets go.

The resolution neither of us has built yet is the third option from this evening's list: **keep the
editor and RE-HOME its view** - when the V2 stops showing it, the Link puts the view back inside its own
`NSViewComponent` (in the Link's image, where that is legal), rather than the editor being destroyed or
the view left behind. That is one move, between two live windows, by the object that owns both sides.

**It needs Sean's word, because it is a change to §2 of the approved design** (which says the editor is
destroyed on return), and because it is the fourth attempt at this door: the honest alternative is to
**park the real editor** and keep the copy road, which now does everything he asked of it.

## 4. If it is taken up again, the order to do it in

1. `lenderRehomeSlotView(slot)` on the lender, called by the borrower *before* it lets go; the Link
   moves the view back into its own wrapper with one `addSubview`, never through a nil window.
2. A rig row for **one press = one take**: the pane opens twice per press today (selection, then the
   deliberate open), and each extra open is an extra take/return on a real plugin. That churn should
   not exist whatever the policy.
3. RE10-RE15 as written (knob, output, slot replace, hand-back both ways three laps) - they are in the
   tree and they fail honestly rather than skipping.
