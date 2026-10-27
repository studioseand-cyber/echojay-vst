// NativeClip.mm — Cocoa helpers for inline-hosted plugin editors
// Compiled as Obj-C++; does NOT include JuceHeader (avoids Component ambiguity).
//
// JUCE attaches a hosted AU plugin's NSView (the AUCocoaView) DIRECTLY to the
// editor window's peer NSView, and its hosting component auto-sizes to the
// plugin. Masking the plugin view itself is useless — its bounds ARE the
// oversized frame — which is how a plugin can end up covering the whole
// window or spilling out of the display area.
//
// Fix: a dedicated container NSView whose frame is LOCKED to the display-area
// rectangle. The container's size comes from OUR layout only and never tracks
// the plugin. wantsLayer + masksToBounds go on the CONTAINER; the plugin view
// is reparented INSIDE it at native size and the container trims overflow.
// Alignment within the (non-flipped) container: innerX centres horizontally,
// innerY = containerH - pluginH pins the plugin's TOP edge to the container
// top (AppKit bottom-left origin).

#import <Cocoa/Cocoa.h>
#include <cstdint>
#include <cmath>
#include "EchoJayFileLog.h"   // 21p item 4: every log line also lands in the plugin's own rolling file
#include "EJShieldWork.h"   // shieldWorkFor: re-assert only what is wrong (his 17:25 glitch)

// Per-target identity: both plugins compile this file into their own dylib
// and can coexist in one hosting process. The ObjC class name MUST be unique
// per target (duplicate names make isKindOfClass unreliable across dylibs),
// and the log tag distinguishes hosts in the unified log.
#ifndef ECHOJAY_CLIP_CLASS
#define ECHOJAY_CLIP_CLASS EchoJayClipContainer
#endif
#ifndef ECHOJAY_SHIELD_CLASS
#define ECHOJAY_SHIELD_CLASS EchoJayViewShield
#endif
#ifndef NATIVECLIP_LOG_TAG
#define NATIVECLIP_LOG_TAG "NativeClip2"
#endif

// Identifiable container class — frame is set exclusively by NativeClip2_attach.
// hadPlugin tracks presence across calls so a LATE-arriving plugin view
// (e.g. ADPTR MetricAB attaches well after editor creation) logs the moment
// it is first captured.
@interface ECHOJAY_CLIP_CLASS : NSView
@property (nonatomic) BOOL hadPlugin;
@end
@implementation ECHOJAY_CLIP_CLASS
// Flipped to match the JUCE peer view (top-left origin). Combined with
// boundsOrigin == frame.origin (set in attach), any frame JUCE writes in
// peer-absolute coordinates renders at the SAME visual position inside the
// container — JUCE's rewrites and our alignment produce identical results,
// so there is no coordinate-space war between the two.
- (BOOL)isFlipped { return YES; }
@end

// ---- THE READ-ONLY SHIELD (his ruling of 24 Oct 2026) ------------------------------------------
//
// *"until control exists, the view blocks input and says so on the view."*
//
// It has to be an NSView, and that is the whole reason this lives here. The live view's own code
// already called setInterceptsMouseClicks (false, false) on the editor component, and its note said
// what that does not reach: a guest AU draws its own NSView, which is a sibling in the window and
// is never under JUCE's mouse handling at all. The Tube-Tech is one of those
// (CSoftubeAUInterfaceCocoaView), so his knobs moved. A transparent view of ours, ABOVE the
// plugin's inside the same container, is the only thing that actually swallows those events.
//
// It also carries the sentence, drawn here rather than in JUCE, for the same reason: anything JUCE
// paints inside the display area is behind the container and would be covered by the plugin.
@interface ECHOJAY_SHIELD_CLASS : NSView
@property (nonatomic, copy) NSString* caption;
@end
@implementation ECHOJAY_SHIELD_CLASS
- (BOOL)isFlipped { return YES; }
// EVERY point inside is ours, so no event reaches the plugin view beneath.
- (NSView*)hitTest:(NSPoint)where
{
    const NSPoint local = [self convertPoint:where fromView:[self superview]];
    return NSPointInRect(local, [self bounds]) ? self : nil;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)e        { (void)e; return YES; }
- (void)mouseDown:(NSEvent*)e                { (void)e; }
- (void)mouseUp:(NSEvent*)e                  { (void)e; }
- (void)mouseDragged:(NSEvent*)e             { (void)e; }
- (void)rightMouseDown:(NSEvent*)e           { (void)e; }
- (void)rightMouseUp:(NSEvent*)e             { (void)e; }
- (void)otherMouseDown:(NSEvent*)e           { (void)e; }
- (void)otherMouseUp:(NSEvent*)e             { (void)e; }
- (void)scrollWheel:(NSEvent*)e              { (void)e; }
- (void)magnifyWithEvent:(NSEvent*)e         { (void)e; }
- (void)rotateWithEvent:(NSEvent*)e          { (void)e; }
- (void)keyDown:(NSEvent*)e                  { (void)e; }
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if ([self.caption length] == 0) return;
    // One bar along the TOP of the plugin's own rect, dark enough to read against any interface and
    // short enough to cover nothing that matters.
    const CGFloat barH = 20.0;
    NSRect bar = NSMakeRect(0, 0, [self bounds].size.width, barH);
    [[NSColor colorWithSRGBRed:0.03 green:0.04 blue:0.07 alpha:0.92] setFill];
    NSRectFillUsingOperation(bar, NSCompositingOperationSourceOver);
    [[NSColor colorWithSRGBRed:0.13 green:0.83 blue:0.93 alpha:0.55] setFill];
    NSRectFillUsingOperation(NSMakeRect(0, barH - 1.0, bar.size.width, 1.0),
                             NSCompositingOperationSourceOver);

    NSMutableParagraphStyle* ps = [[NSMutableParagraphStyle alloc] init];
    [ps setAlignment:NSTextAlignmentCenter];
    NSDictionary* attrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:11.5 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: [NSColor colorWithSRGBRed:0.13 green:0.83 blue:0.93 alpha:1.0],
        NSParagraphStyleAttributeName: ps
    };
    [self.caption drawInRect:NSMakeRect(6, 3, bar.size.width - 12, barH - 5) withAttributes:attrs];
}
@end

static ECHOJAY_SHIELD_CLASS* NativeClip2_findShield(NSView* container)
{
    for (NSView* sv in [container subviews])
        if ([sv isKindOfClass:[ECHOJAY_SHIELD_CLASS class]])
            return (ECHOJAY_SHIELD_CLASS*)sv;
    return nil;
}

extern "C"
{

// Unified-log bridge for cross-platform C++ callers (EchoJayAPI diagnostics)
// — NSLog lands in Console.app / `log stream` like the NativeClip2 lines.
void EchoJay_NSLog(const char* msg)
{
    NSLog(@"%s", msg != nullptr ? msg : "(null)");
    // 21p item 4 (23 Sep 2026): and into the plugin's OWN rolling log, because the unified store dropped every
    // EchoJay line of the session that mattered (23 Sep noon) and the answer had to be reasoned instead of read.
    echojay::FileLog::instance().write (msg != nullptr ? msg : "(null)");
}

static ECHOJAY_CLIP_CLASS* NativeClip2_findContainer(NSView* peer)
{
    for (NSView* sv in [peer subviews])
        if ([sv isKindOfClass:[ECHOJAY_CLIP_CLASS class]])
            return (ECHOJAY_CLIP_CLASS*)sv;
    return nil;
}

// A plugin view attached at peer/window level: any direct subview that is not
// the JUCE peer view class, not a GL view and not our container. Last match
// wins (the hosted editor view is the most recently added).
//
// IMPORTANT: only the peer class prefix "JUCEView" is excluded. Plugins that
// come through JUCE's view-controller hosting path (e.g. ADPTR MetricAB) are
// wrapped in a JUCE-generated "JUCEAUView_<hash>" container — that wrapper IS
// the plugin view for our purposes and must be accepted, not skipped.
static NSView* NativeClip2_findStrayPluginView(NSView* peer, NSView* container)
{
    NSView* found = nil;
    for (NSView* sv in [peer subviews])
    {
        if (sv == container) continue;
        if ([sv isKindOfClass:[ECHOJAY_CLIP_CLASS class]]) continue;
        NSString* cls = NSStringFromClass([sv class]);
        if ([cls hasPrefix:@"JUCEView"])   // the peer NSView class itself
            continue;
        if ([cls rangeOfString:@"OpenGL" options:NSCaseInsensitiveSearch].location != NSNotFound)
            continue;
        found = sv;
    }
    return found;
}

/** THE GUEST VIEW INSIDE THE CONTAINER, which is NOT simply subviews[0] (26 Oct 2026).
    Our own shield is a subview too, and since the stale-view sweep stopped removing it the guest
    can sit at index 1 - at which point subviews[0] is the shield and every "the plugin's view"
    read would have been about our own overlay. One accessor, so the readers cannot drift. */
static NSView* NativeClip2_guestInContainer(NSView* container)
{
    if (container == nil) return nil;
    for (NSView* sv in [container subviews])
        if (! [sv isKindOfClass:[ECHOJAY_SHIELD_CLASS class]]) return sv;
    return nil;
}

static NSView* NativeClip2_pluginViewIn(NSView* peer, NSView* container)
{
    if (NSView* g = NativeClip2_guestInContainer (container)) return g;
    return NativeClip2_findStrayPluginView(peer, container);
}

static NSString* NativeClip2_listSubviews(NSView* v)
{
    NSMutableString* s = [NSMutableString string];
    for (NSView* sv in [v subviews])
        [s appendFormat:@"[%@ %@] ", NSStringFromClass([sv class]),
                        NSStringFromRect([sv frame])];
    return [s length] > 0 ? s : [NSMutableString stringWithString:@"(none)"];
}

// Ensures the clipping container exists with frame (cx, cy, cw, ch) given in
// TOP-LEFT coordinates of the peer view, reparents any peer-level plugin view
// into it, and re-centres the plugin view. The container is NEVER sized from
// the plugin. Returns true once a plugin view is inside the container.
bool NativeClip2_attach(void* peerHandle, int cx, int cy, int cw, int ch, bool doLog,
                        int desiredW, int desiredH, void* expectedGuestView)
{
    if (!peerHandle) return false;
    NSView* peer = (__bridge NSView*)peerHandle;

    // Convert top-left rect to the peer's coordinate system (JUCE peer views
    // are flipped, but don't assume it)
    CGFloat py = [peer isFlipped] ? (CGFloat)cy
                                  : [peer bounds].size.height - (CGFloat)cy - (CGFloat)ch;
    NSRect wanted = NSMakeRect((CGFloat)cx, py, (CGFloat)cw, (CGFloat)ch);

    ECHOJAY_CLIP_CLASS* container = NativeClip2_findContainer(peer);
    if (!container)
    {
        container = [[ECHOJAY_CLIP_CLASS alloc] initWithFrame:wanted];
        [container setAutoresizingMask:NSViewNotSizable]; // never auto-resize
        [container setAutoresizesSubviews:NO];            // never resize the plugin
        [peer addSubview:container];
    }

    // Frame is LOCKED to our layout — reassert every call in case anything
    // (autoresizing, the plugin, JUCE) touched it. The bounds origin mirrors
    // the frame origin so child frames are expressed in peer-absolute
    // coordinates — identical to what JUCE's attachment writes.
    // Card styling on the clip layer itself: dark panel background (unused
    // space must not render white), subtle cyan border, rounded corners.
    // masksToBounds + cornerRadius clips native content to the rounded rect.
    static CGColorRef bgColour =
        CGColorCreateSRGB(0x08 / 255.0, 0x0A / 255.0, 0x12 / 255.0, 1.0);
    static CGColorRef borderColour =
        CGColorCreateSRGB(0x22 / 255.0, 0xd3 / 255.0, 0xee / 255.0, 0.3);
    // ---- A STEADY TICK CHANGES NOTHING (his overnight item 1b, 26 Oct 2026) --------------
    //
    // These seven lines used to run unconditionally, ten times a second, on the layer that holds
    // the hosted AU's own view - and every CALayer property assignment marks the layer as needing
    // display. That continuous recomposite is the wobble. "Reassert every call in case anything
    // touched it" is kept exactly: re-asserting is putting it back when it is WRONG, which is a
    // comparison. The decision is echojay::clipWorkFor, pure and guarded.
    CALayer* clayer = [container layer];
    const bool styleMatches = clayer != nil
        && CGColorEqualToColor ([clayer backgroundColor], bgColour)
        && CGColorEqualToColor ([clayer borderColor], borderColour)
        && [clayer borderWidth] == 1.0
        && [clayer cornerRadius] == 8.0;
    const auto cw0 = echojay::clipWorkFor (NSEqualRects ([container frame], wanted),
                                           NSEqualPoints ([container bounds].origin, wanted.origin),
                                           [container wantsLayer] && clayer != nil,
                                           clayer != nil && [clayer masksToBounds],
                                           styleMatches,
                                           /*havePlugin*/ false, /*pluginFrameMatches*/ true);
    if (cw0.setContainerFrame) [container setFrame:wanted];
    if (cw0.setBoundsOrigin)   [container setBoundsOrigin:wanted.origin];
    if (cw0.setWantsLayer)   { [container setWantsLayer:YES]; clayer = [container layer]; }
    if (cw0.setMasksToBounds)  clayer.masksToBounds = YES;
    if (cw0.setStyle)
    {
        clayer.backgroundColor = bgColour;
        clayer.borderColor     = borderColour;
        clayer.borderWidth     = 1.0;
        clayer.cornerRadius    = 8.0;
    }

    // Collect ALL peer-level stray views. The newest (last-added) is the
    // current editor's view; adopt it into the clip container. Anything older
    // — including a previous editor's placeholder still sitting in the
    // container — is an orphan whose processor is gone: remove it from the
    // hierarchy entirely. Returning orphans to the peer (pre-2.9.22) left
    // them rendering unclipped (the floating "EchoJay V2" placeholder strip).
    NSMutableArray<NSView*>* strays = [NSMutableArray array];
    for (NSView* sv in [[peer subviews] copy])
    {
        if (sv == container) continue;
        if ([sv isKindOfClass:[ECHOJAY_CLIP_CLASS class]]) continue;
        NSString* cls = NSStringFromClass([sv class]);
        if ([cls hasPrefix:@"JUCEView"]) continue;
        if ([cls rangeOfString:@"OpenGL" options:NSCaseInsensitiveSearch].location != NSNotFound)
            continue;
        [strays addObject:sv];
    }
    // ---- WHOSE VIEW IS IT? (his 11:20 fault, 26 Oct 2026) --------------------------------------
    //
    // *"The live view's title said 'Tube-Tech CL 1B' but showed Apple Delay's panel (the deleted
    //  plugin)... A view must only ever be shown in the slot that owns it, and a parked instance's
    //  view must never be reparented anywhere."*
    //
    // "The newest unknown subview of the peer" was a GUESS, and this is what it cost: he deleted
    // the AUDelay, added a CL 1B in its place and opened it, the parked AUDelay's own view was
    // still sitting at peer level, and we adopted it into the CL 1B's container -
    //
    //   11:21:10.035  EJPane(main): open: Tube-Tech CL 1B
    //   11:21:10.038  NativeClip2: reparenting stray AUDelayCustomView ... from peer into container
    //
    // When the caller can name the view JUCE gave THIS editor, the guess is not made at all: that
    // view is adopted and no other is touched. An AU's editor holds it in a juce::NSViewComponent,
    // so the caller can; a VST3's does not, and that road keeps the search, with its guess said out
    // loud in the log rather than presented as a fact.
    NSView* expected = (__bridge NSView*) expectedGuestView;
    NSView* newest = nil;
    if (expected != nil)
    {
        newest = expected;
        if ([strays containsObject:expected]) [strays removeObject:expected];
    }
    else
    {
        newest = [strays lastObject];
        if (newest != nil)
            NSLog(@NATIVECLIP_LOG_TAG ": the caller could not name this editor's own view, so the "
                  @"newest peer-level view (%@) is being ADOPTED ON A GUESS - correct for a VST3 "
                  @"editor, and the one road where a parked plugin's view could still be shown "
                  @"under another slot's name", NSStringFromClass([newest class]));
    }
    for (NSView* sv in strays)
        if (sv != newest)
        {
            // OUT OF THE HIERARCHY, NEVER INTO ANOTHER SLOT'S CONTAINER. A view left at peer level
            // renders unclipped over our own interface (the floating "EchoJay V2" strip, pre-2.9.22),
            // and it may belong to a PARKED instance - so it is removed from the window and adopted
            // nowhere, which is his rule of 26 Oct exactly.
            NSLog(@NATIVECLIP_LOG_TAG ": removing orphaned peer view %@ frame=%@ - it is not this "
                  @"editor's own view and is adopted nowhere",
                  NSStringFromClass([sv class]), NSStringFromRect([sv frame]));
            [sv removeFromSuperview];
        }
    if (newest)
    {
        // A newer view supersedes whatever the container holds (e.g. a real
        // plugin view arriving after an out-of-process placeholder)
        for (NSView* old in [[container subviews] copy])
        {
            if (old == newest) continue;                 // already where it belongs
            if ([old isKindOfClass:[ECHOJAY_SHIELD_CLASS class]]) continue;   // ours, re-asserted below
            NSLog(@NATIVECLIP_LOG_TAG ": replacing stale container view %@ frame=%@",
                  NSStringFromClass([old class]), NSStringFromRect([old frame]));
            [old removeFromSuperview];
        }
        const NSSize keep = [newest frame].size;   // keep the plugin's NATIVE size
        if ([newest superview] != container)
        {
            NSLog(@NATIVECLIP_LOG_TAG ": putting %@ (this editor's %@ view) frame=%@ into the container",
                  NSStringFromClass([newest class]),
                  expected != nil ? @"OWN" : @"newest-stray",
                  NSStringFromRect([newest frame]));
            // ---- THE MOVE MUST NOT TAKE IT OUT OF THE WINDOW (his AUDelay crash, 26 Oct 2026) ----
            //
            // `removeFromSuperview` followed by `addSubview:` is two moves, and the gap between
            // them has the guest view at superview nil - and therefore at WINDOW nil. An AppKit
            // probe says so plainly: with the remove it gets viewWillMoveToWindow:NIL /
            // viewDidMoveToWindow (nil) and then the window again; with addSubview: ALONE it is
            // reparented and never once sees a nil window, because AppKit keeps the window when
            // both superviews are in the same one, which ours always are.
            //
            // WHAT THAT COST. Apple's own AU views give up their parameter connection on the way
            // out of a window, and the next draw asserts reading a parameter:
            //
            //    07:22:31.890  NativeClip2: reparenting stray AUDelayCustomView ... from peer
            //    07:22:31.914  *** Assertion failure in AUDelayCustomView.mm:176   (in drawRect)
            //    07:22:31.944  EXC_BREAKPOINT in -[NSApplication _crashOnException:]
            //
            // 24 ms, and in Logic the AU's UI lives in a view-service process where an exception
            // out of drawRect is fatal - so clicking an AUDelay slot card took Logic's whole
            // hosting process with it. One move instead of two, and the transition never happens.
            // Row LV1 holds it in the host rig, on the real plugin.
            [newest setAutoresizingMask:NSViewNotSizable];
            [container addSubview:newest];
        }
        [newest setFrame:NSMakeRect(wanted.origin.x, wanted.origin.y,
                                    keep.width, keep.height)];
    }

    NSView* plugin = NativeClip2_guestInContainer (container);

    // First capture after an absent stretch (late-attaching plugin view) —
    // force the full diagnostic log so live streams show the transition.
    if (plugin != nil && ![container hadPlugin])
        doLog = true;
    [container setHadPlugin:(plugin != nil)];
    if (plugin)
    {
        NSRect f = [plugin frame];

        // HYPOTHESIS A (out-of-process proxies, e.g. WaveShell's
        // AUv2ContainerView): remote view services can wait for the HOST to
        // impose a size instead of self-sizing. If the view is still
        // degenerate and the caller supplied the editor's reported preferred
        // size, impose it and let the service render at that size.
        if (desiredW > 100 && desiredH > 60
            && (f.size.width <= 100 || f.size.height <= 60))
        {
            NSLog(@NATIVECLIP_LOG_TAG ": imposing %dx%d on degenerate %@ (was %@)",
                  desiredW, desiredH, NSStringFromClass([plugin class]),
                  NSStringFromRect(f));
            f.size.width  = (CGFloat)desiredW;
            f.size.height = (CGFloat)desiredH;
            [plugin setAutoresizingMask:NSViewNotSizable];
        }

        // FINAL sizing policy: native size, horizontally centred, TOP-PINNED
        // vertically — the plugin's top edge is always visible and overflow
        // trims off the BOTTOM only. The container is flipped with
        // boundsOrigin == frame.origin, so child coordinates are peer-absolute
        // top-left: top pin = the container's own y origin.
        f.origin.x = wanted.origin.x + std::floor(((CGFloat)cw - f.size.width) * 0.5);
        f.origin.y = wanted.origin.y;
        if (!NSEqualRects([plugin frame], f))
            [plugin setFrame:f];
    }

    if (doLog)
    {
        // Full diagnostic line — coordinate conventions, computed alignment,
        // and complete subview inventories of both the peer and the container
        NSRect pf = plugin ? [plugin frame] : NSZeroRect;
        NSLog(@NATIVECLIP_LOG_TAG "-diag: container=%@ flipped=%d masksToBounds=%d | peer=%@ flipped=%d | plugin=%@ frame=%@ | computed innerX=%d innerY=%d | peerSubviews: %@| containerSubviews: %@",
              NSStringFromRect([container frame]),
              (int)[container isFlipped],
              (int)[container layer].masksToBounds,
              NSStringFromClass([peer class]),
              (int)[peer isFlipped],
              plugin ? NSStringFromClass([plugin class]) : @"(none)",
              plugin ? NSStringFromRect(pf) : @"-",
              plugin ? (int)pf.origin.x : 0,
              plugin ? (int)pf.origin.y : 0,
              NativeClip2_listSubviews(peer),
              NativeClip2_listSubviews(container));

        // Nothing captured — dump the window content view TWO levels deep and
        // enumerate this process's other windows, so we can see WHERE the
        // plugin view went (out-of-process editors may make their own window).
        if (!plugin)
        {
            NSMutableString* wdump = [NSMutableString string];
            NSView* wcv = [[peer window] contentView];
            if (wcv)
                for (NSView* sv in [wcv subviews])
                {
                    [wdump appendFormat:@"[%@ %@", NSStringFromClass([sv class]),
                                        NSStringFromRect([sv frame])];
                    if ([[sv subviews] count] > 0)
                    {
                        [wdump appendString:@" {"];
                        for (NSView* s2 in [sv subviews])
                            [wdump appendFormat:@"[%@ %@] ", NSStringFromClass([s2 class]),
                                                NSStringFromRect([s2 frame])];
                        [wdump appendString:@"}"];
                    }
                    [wdump appendString:@"] "];
                }
            NSMutableString* windows = [NSMutableString string];
            for (NSWindow* w in [NSApp windows])
                if (w != [peer window])
                    [windows appendFormat:@"[%@ \"%@\" cv=%@ %@] ",
                        NSStringFromClass([w class]), [w title],
                        NSStringFromClass([[w contentView] class]),
                        NSStringFromRect([w frame])];
            NSLog(@NATIVECLIP_LOG_TAG "-diag: no plugin view. window contentView tree: %@| other windows: %@",
                  [wdump length] > 0 ? wdump : @"(none) ",
                  [windows length] > 0 ? windows : @"(none)");
        }
    }

    return plugin != nil;
}

// Reads the ACTUAL frame of the plugin view (inside the container, or still
// stray at peer level). JUCE-reported editor sizes are unreliable; this is
// ground truth. Degenerate results (1x1 etc.) mean "not ready — retry".
bool NativeClip2_getPluginSizeVerbose(void* peerHandle, int* outW, int* outH,
                                      char* desc, int descLen)
{
    if (desc && descLen > 0) desc[0] = 0;
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!peerHandle) return false;
    NSView* peer = (__bridge NSView*)peerHandle;
    ECHOJAY_CLIP_CLASS* container = NativeClip2_findContainer(peer);
    NSView* inContainer = NativeClip2_guestInContainer (container);
    NSView* plugin = inContainer != nil
                         ? inContainer
                         : NativeClip2_findStrayPluginView(peer, container);
    NSRect cf = container != nil ? [container frame] : NSZeroRect;
    if (!plugin)
    {
        if (desc)
            snprintf(desc, (size_t)descLen,
                     "no plugin view; container=%s %dx%d subviews=%d",
                     container ? "yes" : "NO",
                     (int)cf.size.width, (int)cf.size.height,
                     container ? (int)[[container subviews] count] : -1);
        return false;
    }
    NSRect f = [plugin frame];
    if (outW) *outW = (int)f.size.width;
    if (outH) *outH = (int)f.size.height;
    if (desc)
        snprintf(desc, (size_t)descLen,
                 "measured=%s %dx%d@(%d,%d) source=%s container=%dx%d subviews=%d",
                 [NSStringFromClass([plugin class]) UTF8String],
                 (int)f.size.width, (int)f.size.height,
                 (int)f.origin.x, (int)f.origin.y,
                 inContainer != nil ? "container[0]" : "peer-STRAY",
                 (int)cf.size.width, (int)cf.size.height,
                 container ? (int)[[container subviews] count] : -1);
    return true;
}

bool NativeClip2_getPluginSize(void* peerHandle, int* outW, int* outH)
{
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!peerHandle) return false;
    NSView* peer = (__bridge NSView*)peerHandle;
    NSView* plugin = NativeClip2_pluginViewIn(peer, NativeClip2_findContainer(peer));
    if (!plugin) return false;
    NSRect f = [plugin frame];
    if (outW) *outW = (int)f.size.width;
    if (outH) *outH = (int)f.size.height;
    return true;
}

// Removes the container after the hosted editor has been destroyed. JUCE's
// teardown already pulled its own view out; anything still inside is an
// orphan (its processor is gone) — remove it from the hierarchy entirely.
// Do NOT hand leftovers back to the peer: an unclipped orphan at peer level
// renders over the UI (the "EchoJay V2" placeholder-strip bug).
// ---- PUT THE SHIELD UP, OR TAKE IT DOWN (his ruling of 24 Oct 2026) ---------------------------
//
// Called after every attach, because attach reparents the plugin view and the shield has to stay
// the LAST subview to stay on top. Sized to the plugin's own view when there is one, so the
// sentence sits on the interface rather than floating in the card's margin; the container's whole
// bounds otherwise, which is what a plugin that has not drawn yet needs.
//
// Returns true when a shield is up afterwards.
bool NativeClip2_setShield(void* peerHandle, bool on, const char* caption)
{
    if (!peerHandle) return false;
    NSView* peer = (__bridge NSView*)peerHandle;
    ECHOJAY_CLIP_CLASS* container = NativeClip2_findContainer(peer);
    if (!container) return false;

    ECHOJAY_SHIELD_CLASS* shield = NativeClip2_findShield(container);
    if (!on)
    {
        if (shield) [shield removeFromSuperview];
        return false;
    }

    // WHICH RECT? The plugin's own view, when it is in the container.
    NSRect want = [container bounds];
    for (NSView* sv in [container subviews])
    {
        if ([sv isKindOfClass:[ECHOJAY_SHIELD_CLASS class]]) continue;
        const NSRect f = [sv frame];
        if (f.size.width > 40.0 && f.size.height > 30.0) { want = f; break; }
    }

    // ---- ONLY WHAT IS ACTUALLY WRONG (his 17:25 glitch, 25 Oct 2026) ---------------------
    //
    // This block used to set the caption, remove the shield from its superview, add it back on
    // top and force a redraw on EVERY call - and the panel's timer calls this ten times a second
    // through attachNative. A sibling view over a hosted AU's NSView leaving and rejoining the
    // hierarchy at 10 Hz recomposites that whole region, which is what he saw glitching on the
    // plugin's window and on the slot meters drawn in the same card.
    //
    // The decision is echojay::shieldWorkFor, pure and guarded; this only carries it out.
    NSString* wantCap = (caption != NULL ? [NSString stringWithUTF8String:caption] : @"");
    const bool isTopmost = ([[container subviews] lastObject] == shield);
    const echojay::ShieldWork work =
        echojay::shieldWorkFor (/*on*/ true,
                                /*exists*/ shield != nil,
                                /*frameMatches*/ shield != nil && NSEqualRects ([shield frame], want),
                                /*captionMatches*/ shield != nil && [[shield caption] isEqualToString: wantCap],
                                isTopmost);
    if (work.create)
    {
        shield = [[ECHOJAY_SHIELD_CLASS alloc] initWithFrame:want];
        [shield setAutoresizingMask:NSViewNotSizable];
        [container addSubview:shield];
        NSLog(@NATIVECLIP_LOG_TAG ": the read-only shield is UP over %@ - the live view takes no "
              @"gestures until control through it is built", NSStringFromRect(want));
    }
    if (work.setFrame)   [shield setFrame:want];
    if (work.setCaption) [shield setCaption:wantCap];
    if (work.reorder)
    {
        // The case the old comment named, now DETECTED rather than assumed every tick: a re-attach
        // added the plugin's view after ours, so the shield is no longer last and blocks nothing.
        [shield removeFromSuperview];
        [container addSubview:shield positioned:NSWindowAbove relativeTo:nil];
        NSLog(@NATIVECLIP_LOG_TAG ": the shield was no longer topmost (a re-attach) - put back on top");
    }
    if (work.redraw) [shield setNeedsDisplay:YES];
    return true;
}

// ---- THE VIEW GOES HOME, IN ONE MOVE (his ruling of 26 Oct 21:08) -----------------------------
//
// A lent editor is KEPT across a hand-back (Apple's AUDelay view does not survive being built and
// destroyed repeatedly on one long-lived instance), so when the borrower lets go its view must leave
// the borrower's window without being destroyed and without being stranded there. It is moved into a
// parking view this bundle owns: one addSubview, no removeFromSuperview first, which is the rule this
// road has paid for twice. AppKit re-parents in place; the view keeps its superview throughout.
//
// The parking view is deliberately windowless and never drawn. A view in it is a view nobody is
// showing, which is exactly its state: retained, intact, and ready to be moved back into a container
// by attach() the next time somebody opens it.
static NSView* NativeClip2_parkingView(void)
{
    static NSView* parking = nil;
    if (parking == nil)
    {
        parking = [[NSView alloc] initWithFrame: NSMakeRect(0, 0, 4000, 4000)];
        [parking setHidden: YES];
        NSLog(@NATIVECLIP_LOG_TAG ": made the parking view for guests nobody is showing");
    }
    return parking;
}

bool NativeClip2_parkGuestView(void* guestView)
{
    if (!guestView) return false;
    NSView* guest = (__bridge NSView*)guestView;
    NSView* parking = NativeClip2_parkingView();
    if (parking == nil) return false;
    if ([guest superview] == parking) return true;          // already home
    @try
    {
        [guest setAutoresizingMask: NSViewNotSizable];
        [parking addSubview: guest];                         // ONE move; nothing removed first
    }
    @catch (NSException* e)
    {
        NSLog(@NATIVECLIP_LOG_TAG ": parking %@ threw %@", NSStringFromClass([guest class]), [e name]);
        return false;
    }
    NSLog(@NATIVECLIP_LOG_TAG ": parked %@ (its editor is kept; window=%@)",
          NSStringFromClass([guest class]), [guest window] == nil ? @"none, as intended" : @"still one");
    return [guest superview] == parking;
}

// ---- HIDDEN, NOT TAKEN APART (round 2 option 3, 26 Oct 2026) ----------------------------------
//
// His ruling: *"a lent editor moves into the V2's window once and is hidden, not detached, on
// close."*  The reason is the crash this codebase has now paid for twice: moving a hosted AU's
// NSView to a nil window while the view goes on living. detach() below removes every child from the
// container - which is right when the editor is about to be destroyed with it, and fatal when the
// editor survives. So a view that is coming back is HIDDEN where it stands: same superview, same
// window, nothing moves.
void NativeClip2_setHidden(void* peerHandle, bool hidden)
{
    if (!peerHandle) return;
    NSView* peer = (__bridge NSView*)peerHandle;
    ECHOJAY_CLIP_CLASS* container = NativeClip2_findContainer(peer);
    if (!container) return;
    [container setHidden: hidden ? YES : NO];
    NSLog(@NATIVECLIP_LOG_TAG ": container %@ (its %lu subview(s) stay where they are)",
          hidden ? @"HIDDEN" : @"shown", (unsigned long) [[container subviews] count]);
}

// ---- AND THE CONTAINER ALONE, ONCE ITS GUEST IS ALREADY GONE ----------------------------------
// The other half of the same rule. When a lent editor is handed back, its OWNER destroys it, and
// that takes the plugin's view with it in the way AppKit expects (a dealloc of the hierarchy, not a
// hop to nil). What is left is an empty container of ours, and this removes exactly that - it never
// touches a child, because a child here would be a view that is still alive.
void NativeClip2_detachEmptyContainer(void* peerHandle)
{
    if (!peerHandle) return;
    NSView* peer = (__bridge NSView*)peerHandle;
    ECHOJAY_CLIP_CLASS* container = NativeClip2_findContainer(peer);
    if (!container) return;
    const unsigned long left = (unsigned long) [[container subviews] count];
    if (left > 0)
    {
        // Said out loud rather than cleaned up: a view still in here means the editor was NOT
        // destroyed first, which is the ordering this exists to keep.
        NSLog(@NATIVECLIP_LOG_TAG ": REFUSING to remove the container - %lu view(s) are still inside "
              @"it, so its guest outlived the editor it belongs to", left);
        return;
    }
    [container removeFromSuperview];
}

void NativeClip2_detach(void* peerHandle)
{
    if (!peerHandle) return;
    NSView* peer = (__bridge NSView*)peerHandle;
    ECHOJAY_CLIP_CLASS* container = NativeClip2_findContainer(peer);
    if (!container) return;
    for (NSView* child in [[container subviews] copy])
    {
        NSLog(@NATIVECLIP_LOG_TAG ": detach removing leftover view %@ frame=%@",
              NSStringFromClass([child class]), NSStringFromRect([child frame]));
        [child removeFromSuperview];
    }
    [container removeFromSuperview];
}

} // extern "C"
