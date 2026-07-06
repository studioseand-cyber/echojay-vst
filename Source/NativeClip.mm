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

// Identifiable container class — frame is set exclusively by NativeClip2_attach.
// hadPlugin tracks presence across calls so a LATE-arriving plugin view
// (e.g. ADPTR MetricAB attaches well after editor creation) logs the moment
// it is first captured.
@interface EchoJayClipContainer : NSView
@property (nonatomic) BOOL hadPlugin;
@end
@implementation EchoJayClipContainer
// Flipped to match the JUCE peer view (top-left origin). Combined with
// boundsOrigin == frame.origin (set in attach), any frame JUCE writes in
// peer-absolute coordinates renders at the SAME visual position inside the
// container — JUCE's rewrites and our alignment produce identical results,
// so there is no coordinate-space war between the two.
- (BOOL)isFlipped { return YES; }
@end

extern "C"
{

// Unified-log bridge for cross-platform C++ callers (EchoJayAPI diagnostics)
// — NSLog lands in Console.app / `log stream` like the NativeClip2 lines.
void EchoJay_NSLog(const char* msg)
{
    NSLog(@"%s", msg != nullptr ? msg : "(null)");
}

static EchoJayClipContainer* NativeClip2_findContainer(NSView* peer)
{
    for (NSView* sv in [peer subviews])
        if ([sv isKindOfClass:[EchoJayClipContainer class]])
            return (EchoJayClipContainer*)sv;
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
        if ([sv isKindOfClass:[EchoJayClipContainer class]]) continue;
        NSString* cls = NSStringFromClass([sv class]);
        if ([cls hasPrefix:@"JUCEView"])   // the peer NSView class itself
            continue;
        if ([cls rangeOfString:@"OpenGL" options:NSCaseInsensitiveSearch].location != NSNotFound)
            continue;
        found = sv;
    }
    return found;
}

static NSView* NativeClip2_pluginViewIn(NSView* peer, NSView* container)
{
    if (container != nil && [[container subviews] count] > 0)
        return [container subviews][0];
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
bool NativeClip2_attach(void* peerHandle, int cx, int cy, int cw, int ch, bool doLog)
{
    if (!peerHandle) return false;
    NSView* peer = (__bridge NSView*)peerHandle;

    // Convert top-left rect to the peer's coordinate system (JUCE peer views
    // are flipped, but don't assume it)
    CGFloat py = [peer isFlipped] ? (CGFloat)cy
                                  : [peer bounds].size.height - (CGFloat)cy - (CGFloat)ch;
    NSRect wanted = NSMakeRect((CGFloat)cx, py, (CGFloat)cw, (CGFloat)ch);

    EchoJayClipContainer* container = NativeClip2_findContainer(peer);
    if (!container)
    {
        container = [[EchoJayClipContainer alloc] initWithFrame:wanted];
        [container setAutoresizingMask:NSViewNotSizable]; // never auto-resize
        [container setAutoresizesSubviews:NO];            // never resize the plugin
        [peer addSubview:container];
    }

    // Frame is LOCKED to our layout — reassert every call in case anything
    // (autoresizing, the plugin, JUCE) touched it. The bounds origin mirrors
    // the frame origin so child frames are expressed in peer-absolute
    // coordinates — identical to what JUCE's attachment writes.
    if (!NSEqualRects([container frame], wanted))
        [container setFrame:wanted];
    [container setBoundsOrigin:wanted.origin];
    [container setWantsLayer:YES];
    [container layer].masksToBounds = YES;
    // Card styling on the clip layer itself: dark panel background (unused
    // space must not render white), subtle cyan border, rounded corners.
    // masksToBounds + cornerRadius clips native content to the rounded rect.
    // (statics: created once, this runs on every 400ms maintenance pass)
    static CGColorRef bgColour =
        CGColorCreateSRGB(0x08 / 255.0, 0x0A / 255.0, 0x12 / 255.0, 1.0);
    static CGColorRef borderColour =
        CGColorCreateSRGB(0x22 / 255.0, 0xd3 / 255.0, 0xee / 255.0, 0.3);
    [container layer].backgroundColor = bgColour;
    [container layer].borderColor     = borderColour;
    [container layer].borderWidth     = 1.0;
    [container layer].cornerRadius    = 8.0;

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
        if ([sv isKindOfClass:[EchoJayClipContainer class]]) continue;
        NSString* cls = NSStringFromClass([sv class]);
        if ([cls hasPrefix:@"JUCEView"]) continue;
        if ([cls rangeOfString:@"OpenGL" options:NSCaseInsensitiveSearch].location != NSNotFound)
            continue;
        [strays addObject:sv];
    }
    NSView* newest = [strays lastObject];
    for (NSView* sv in strays)
        if (sv != newest)
        {
            NSLog(@"NativeClip2: removing orphaned peer view %@ frame=%@",
                  NSStringFromClass([sv class]), NSStringFromRect([sv frame]));
            [sv removeFromSuperview];
        }
    if (newest)
    {
        // A newer view supersedes whatever the container holds (e.g. a real
        // plugin view arriving after an out-of-process placeholder)
        for (NSView* old in [[container subviews] copy])
        {
            NSLog(@"NativeClip2: replacing stale container view %@ frame=%@",
                  NSStringFromClass([old class]), NSStringFromRect([old frame]));
            [old removeFromSuperview];
        }
        NSSize keep = [newest frame].size;   // keep the plugin's NATIVE size
        NSLog(@"NativeClip2: reparenting stray %@ frame=%@ from peer into container",
              NSStringFromClass([newest class]), NSStringFromRect([newest frame]));
        [newest removeFromSuperview];
        [newest setAutoresizingMask:NSViewNotSizable];
        [container addSubview:newest];
        [newest setFrame:NSMakeRect(wanted.origin.x, wanted.origin.y,
                                    keep.width, keep.height)];
    }

    NSView* plugin = ([[container subviews] count] > 0) ? [container subviews][0] : nil;

    // First capture after an absent stretch (late-attaching plugin view) —
    // force the full diagnostic log so live streams show the transition.
    if (plugin != nil && ![container hadPlugin])
        doLog = true;
    [container setHadPlugin:(plugin != nil)];
    if (plugin)
    {
        // FINAL sizing policy: native size, horizontally centred, TOP-PINNED
        // vertically — the plugin's top edge is always visible and overflow
        // trims off the BOTTOM only. The container is flipped with
        // boundsOrigin == frame.origin, so child coordinates are peer-absolute
        // top-left: top pin = the container's own y origin.
        NSRect f = [plugin frame];
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
        NSLog(@"NativeClip2-diag: container=%@ flipped=%d masksToBounds=%d | peer=%@ flipped=%d | plugin=%@ frame=%@ | computed innerX=%d innerY=%d | peerSubviews: %@| containerSubviews: %@",
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

        // Nothing captured — also dump the window content view so we can see
        // WHERE the plugin view went.
        if (!plugin)
        {
            NSMutableString* wdump = [NSMutableString string];
            NSView* wcv = [[peer window] contentView];
            if (wcv && wcv != peer)
                for (NSView* sv in [wcv subviews])
                    [wdump appendFormat:@"[%@ %@] ", NSStringFromClass([sv class]),
                                        NSStringFromRect([sv frame])];
            NSLog(@"NativeClip2-diag: no plugin view. window contentView subviews: %@",
                  [wdump length] > 0 ? wdump : @"(peer is contentView / none)");
        }
    }

    return plugin != nil;
}

// Reads the ACTUAL frame of the plugin view (inside the container, or still
// stray at peer level). JUCE-reported editor sizes are unreliable; this is
// ground truth. Degenerate results (1x1 etc.) mean "not ready — retry".
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
void NativeClip2_detach(void* peerHandle)
{
    if (!peerHandle) return;
    NSView* peer = (__bridge NSView*)peerHandle;
    EchoJayClipContainer* container = NativeClip2_findContainer(peer);
    if (!container) return;
    for (NSView* child in [[container subviews] copy])
    {
        NSLog(@"NativeClip2: detach removing leftover view %@ frame=%@",
              NSStringFromClass([child class]), NSStringFromRect([child frame]));
        [child removeFromSuperview];
    }
    [container removeFromSuperview];
}

} // extern "C"
