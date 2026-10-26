# The live view of a Link's plugin: the whole path, every thread, every timer
### Step (a) of his 26 Oct ruling, written before any code. A831AC56 = commit e697be8b (the build before last night); 5ECD2CDF = fe0ff971 (last night's).

## 1. What is on screen

Clicking a slot card on a Link's rack opens the slot's plugin **as a copy in the V2's own process**
(`shadowBegin`, `PluginProcessor.cpp:4420`). What he sees is **that copy's own editor** - the plugin's
own NSView, drawing its own knobs with its own code. We never draw a knob in the live view. We own
three things around it:

* `EchoJayClipContainer` - a flipped, layer-backed NSView we insert into the JUCE peer, with
  `masksToBounds`, a dark background, a cyan border, 8 px corners, and **`boundsOrigin` set equal to
  its own `frame.origin`** so that child frames can be written in peer-absolute coordinates
  (`NativeClip.mm:200-250`).
* the reparent: JUCE attaches the plugin's view to the **peer**, so on every open we take it out of
  the peer and put it in the container (`NativeClip.mm:269-295`).
* `EchoJayShield` - a sibling view above the plugin's view, up only when input must be blocked.

## 2. A drag, step by step, with the thread for each step

| # | what happens | thread |
|---|---|---|
| 1 | his hand moves a knob **on the copy's own editor** | main (message) |
| 2 | the plugin writes its own parameter and **draws its own knob** | main |
| 3 | `EchoJayLiveControlListener` -> `liveControlParamChanged` (`PluginProcessor.cpp:4844`) | main (bounced from audio if needed) |
| 4 | echo test: equal to what FOLLOW last applied -> dropped | main |
| 5 | policy gate: following, and no run working on the rack | main |
| 6 | `ringBeginGesture` / `ringStreamValue` on that Link's `cmdring` | main |
| 7 | the Link drains the frame and applies it to the **real** plugin | Link's audio thread hands it to the Link's message thread |
| 8 | **he hears it** | Link's audio thread |
| 9 | the Link publishes the tapped slot's values at 30 Hz into `shadowval_<uid>.bin` | Link's message thread |
| 10 | the V2's 20 Hz editor timer -> `shadowFollowTick` -> `followApply` writes the changed values into the copy, skipping any parameter his hand is on | main |
| 11 | the plugin redraws its own knob from that write | main |

**The deduction that matters: he hears the change, so step 3 fired, so the copy's parameter DID
move.** Steps 3-8 cannot run without it. A parameter that moved and a knob that did not move is
therefore not a value fault at all - it is step 2's **drawing** not reaching the screen.

## 3. Every timer that touches the hosted view, its layers, or the copy's parameters

| timer | rate | what it does to the view / the copy |
|---|---|---|
| the pane's own (`PluginEditor.h:4157`) | 100 ms while settling (up to 5 s), then **400 ms** | `attachNative` -> `NativeClip::attach`: container frame, `boundsOrigin`, `wantsLayer`, `masksToBounds`, four layer style properties, the stray scan, the plugin's frame; then `setShield` |
| the editor's main (`PluginEditor.cpp:3631`) | **20 Hz** | `slotMeterTick`; `shadowFollowTick` (**writes the copy's parameters**); `shadowControlTick` (closes an unframed gesture after 150 ms quiet); and ~1,400 lines of other work per tick |
| `GestureTimer` (`PluginProcessor.h:2249`) | 20 Hz | drains hosted gestures |
| the processor's | 1 s | housekeeping |
| lease / rack-lock renewals | 1.5 s-ish | nothing in the view |
| panel animations (MatchPanel and three others) | 60 Hz | only while that panel is visible |
| the Link side | 30 Hz | publishes slot meters and the shadow values both |

Last night (`86d4e5d5`) changed exactly two things in this table: the 400 ms pass now writes the
container's geometry and layer style **only when they are wrong** (`clipWorkFor`), and `followApply`
**skips a parameter his hand is on** (`followMayWriteParam`, 250 ms settle).

## 4. Why each fault happens

### 1. AUDelay crashes Logic on opening the live view
Proven from the report and the system log, not inferred:

```
07:22:31.889  EJPane(main): attempt start: AUDelay
07:22:31.890  NativeClip2: reparenting stray AUDelayCustomView frame={{214,143},{484,255}} from peer into container
07:22:31.914  *** Assertion failure in AUDelayCustomView.mm:176     <- Apple's own view, in drawRect
              [AUDynamicsProcessorCustomView parameterValue]
              ... _NSViewDrawRect -> NSViewBackingLayer display -> CA::Transaction::commit
07:22:31.9449 EXC_BREAKPOINT in -[NSApplication _crashOnException:]
```
Crash report `AUHostingServiceXPC_arrow-2026-10-26-072247.ips`, pid 23964, the process that was
hosting V2 `5ECD2CDF`; 24 ms from our reparent to Apple's assertion, on the first draw after it.

The reparent is `[newest removeFromSuperview]` followed by `[container addSubview:newest]`. The
`removeFromSuperview` takes the guest view to **superview nil, and therefore window nil**, and back.
CoreAudioAUUI's views give up their parameter connection on that transition and then assert when
`drawRect` reads a parameter. **This is not last night's regression** - that code is untouched since
before A831AC56; what is new is that he opened an Apple AU in the live view.

Why the bench passed: `EchoJayShadowRepro` hosts AUDelay with no clip container and no live view, so
it never reparents the guest view - and it is an ordinary app, where AppKit's handler is not
`NSViewServiceApplication`'s. Inside Logic the AU's UI lives in a **view-service process**, where an
uncaught exception out of `drawRect` is fatal rather than logged. The bench could not have caught
this, which is his point about it not testing what he sees.

### 2. The knob does not move while he drags it
By the deduction in §2 the parameter moves, so the picture is stale. What used to refresh it: the
400 ms pass wrote four CALayer properties on the container unconditionally, and **every CALayer
property assignment marks that layer for display**, which recomposited the whole container including
the guest's view. That is why the knob appeared to move in ~400 ms steps before last night, and why
it froze the moment those writes became conditional. His own guess (FOLLOW's write was the only
thing moving it) is the same shape of fault one layer up - and FOLLOW is also now held off his knob,
so both of the things that used to force the picture forward are gone on the same build.

Why the guest's own draws do not reach the screen is the question the test has to answer. The prime
suspect is the container's geometry: a **non-zero `boundsOrigin` on a flipped, layer-backed view**,
which is what maps a child's dirty rect into the layer. An invalidation offset by the bounds origin
lands outside the knob, so the knob is never redrawn, while any whole-layer invalidation (a property
write, a frame change) paints the correct picture at once. That matches every symptom, including the
quarter-second step.

### 3. The wobble with his hands off
Same family, different cause now. The unconditional layer writes are gone, so what is left to
disturb the picture on a timer is: the 400 ms pass finding the container's frame or the plugin's
centred frame **genuinely different** each tick (`displayArea()` moves if the panel's caption or
status line changes height - and the 20 Hz tick rewrites `chainListPanel.statusText` from the
refusal, the follow note and the meters), the shield's frame following the plugin's, and
`EJPanel: rebuild` (seen at 07:22:30 in his own log, driven by a trim moving on the Link). The test
samples the frame and the layer properties every tick and names which one actually moves.

### 4. Laggy, worse than A831AC56
The 20 Hz tick is ~1,400 lines of work per tick before anything is drawn. The live view adds, per
tick: the shadow values read and applied, the per-slot meter read, the gesture close, two status
comparisons that repaint the panel. His own log shows the V2 writing up to 747 lines in one second
through `EchoJay_NSLog` (an `NSLog` plus a file append, on the message thread) around the live view
opening. None of that is measured today, which is the fault to fix first: the test measures frame
time with the live view open, with the Link's own window open instead, and on both builds.

## 5. What the real-host test must sample, every tick

The copy's parameter value; the Link's value for the same parameter; the hosted view's frame; the
container's frame, `boundsOrigin` and layer properties; **the number of times the guest view's own
`drawRect:` ran** (the only measure that can see "the knob did not move on screen" - counted by
swizzling that one class in the rig process, never in the product); and the message thread's tick
time. Subjects: AUDelay, an EchoJay device, and one third-party non-PACE plugin. Each fault shown
red before it is fixed.
