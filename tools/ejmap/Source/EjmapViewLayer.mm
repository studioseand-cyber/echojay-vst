/*
  EjmapViewLayer.mm

  See EjmapViewLayer.h. Measurement first, then the one mutation.
*/

#include "EjmapViewLayer.h"

#if JUCE_MAC
 #include <Cocoa/Cocoa.h>
#endif

namespace ejmap
{

#if JUCE_MAC

namespace
{
    NSView* peerViewOf (juce::Component& c)
    {
        if (auto* peer = c.getPeer())
            return (NSView*) peer->getNativeHandle();
        return nil;
    }

    void describe (NSView* v, int depth, juce::String& out)
    {
        if (v == nil) return;
        const auto f = [v frame];
        out << juce::String::repeatedString ("  ", depth)
            << "- " << juce::String::fromUTF8 ([NSStringFromClass ([v class]) UTF8String])
            << "  frame " << (int) f.origin.x << "," << (int) f.origin.y
            << " " << (int) f.size.width << "x" << (int) f.size.height
            << "  wantsLayer=" << ([v wantsLayer] ? "YES" : "no")
            << "  layer=" << ([v layer] != nil ? "yes" : "nil")
            << "\n";

        // One level of children is enough to see a plugin view and whether it
        // brought a layer with it. Deeper is a plugin's own business.
        if (depth < 2)
            for (NSView* sub in [v subviews])
                describe (sub, depth + 1, out);
    }
}

juce::String describeViewTree (juce::Component& topLevel)
{
    juce::String out;
    NSView* peer = peerViewOf (topLevel);
    if (peer == nil) { return "(no peer)\n"; }
    describe (peer, 0, out);
    return out;
}

juce::String layerBackHostedViews (juce::Component& topLevel)
{
    NSView* peer = peerViewOf (topLevel);
    if (peer == nil)
        return "no peer yet: nothing done (the window must exist first)\n";

    juce::String out;
    int touched = 0;
    for (NSView* sub in [peer subviews])
    {
        const auto cls = juce::String::fromUTF8 ([NSStringFromClass ([sub class]) UTF8String]);
        if ([sub wantsLayer])
        {
            out << "  " << cls << ": already layer-backed, left alone\n";
            continue;
        }
        [sub setWantsLayer: YES];

        // STATE THE OBSERVATION, NOT THE INTENTION. setWantsLayer is a
        // request; AppKit decides when the layer object appears. Reporting
        // "done" without reading it back is the class this project keeps
        // filing.
        out << "  " << cls << ": setWantsLayer:YES -> wantsLayer="
            << ([sub wantsLayer] ? "YES" : "no")
            << ", layer=" << ([sub layer] != nil ? "yes" : "nil") << "\n";
        ++touched;
    }
    if (touched == 0 && out.isEmpty())
        out << "  (no hosted views under the peer: nothing to do)\n";
    return out;
}

void stopPeriodicEventsIfAny()
{
    [NSEvent stopPeriodicEvents];
}

juce::String captureHostedEditor (juce::Component& topLevel, const juce::File& out)
{
    return captureHostedEditorResult (topLevel, out).note;
}

CaptureResult captureHostedEditorResult (juce::Component& topLevel, const juce::File& out)
{
    CaptureResult cr;
    NSView* peer = peerViewOf (topLevel);
    if (peer == nil) { cr.note = "no peer\n"; return cr; }

    NSView* editor = nil;
    for (NSView* sub in [peer subviews])
        if (NSWidth ([sub frame]) > 100 && NSHeight ([sub frame]) > 100)
            editor = sub;                       // the hosted container
    if (editor == nil) { cr.note = "no hosted view under the peer\n"; return cr; }

    const auto r = [editor bounds];
    NSBitmapImageRep* rep = [editor bitmapImageRepForCachingDisplayInRect: r];
    if (rep == nil) { cr.note = "bitmapImageRepForCachingDisplayInRect returned nil\n"; return cr; }
    [editor cacheDisplayInRect: r toBitmapImageRep: rep];

    // HOW MUCH OF IT IS ACTUALLY THERE. A bridged editor's pixels live in
    // another process, so this can succeed and hand back an empty rectangle;
    // reporting "captured" on that would be the class this project keeps
    // filing. Sampled, not exhaustive: every 8th pixel is plenty to tell an
    // empty rect from a drawn one.
    long lit = 0, seen = 0;
    for (NSInteger y = 0; y < [rep pixelsHigh]; y += 8)
        for (NSInteger x = 0; x < [rep pixelsWide]; x += 8)
        {
            NSUInteger px[5] = {0,0,0,0,0};
            [rep getPixel: px atX: x y: y];
            ++seen;
            if (px[0] > 8 || px[1] > 8 || px[2] > 8) ++lit;
        }
    const double frac = seen > 0 ? (double) lit / (double) seen : 0.0;

    NSData* png = [rep representationUsingType: NSBitmapImageFileTypePNG properties: @{}];
    const bool wrote = png != nil
        && [png writeToFile: [NSString stringWithUTF8String: out.getFullPathName().toRawUTF8()] atomically: YES];

    cr.attempted = true;
    cr.wrote     = wrote;
    cr.width     = (int) [rep pixelsWide];
    cr.height    = (int) [rep pixelsHigh];
    cr.fraction  = frac;
    cr.note = juce::String ("captured ") + juce::String (cr.width) + "x"
                + juce::String (cr.height)
                + ", non-background " + juce::String (frac * 100.0, 1) + "%"
                + ", png " + (wrote ? "written" : "FAILED") + "\n";
    return cr;
}

#else

juce::String describeViewTree (juce::Component&)      { return {}; }
juce::String captureHostedEditor (juce::Component&, const juce::File&) { return {}; }
CaptureResult captureHostedEditorResult (juce::Component&, const juce::File&) { return {}; }
void stopPeriodicEventsIfAny() {}
juce::String layerBackHostedViews (juce::Component&)  { return "not macOS: nothing to do\n"; }

#endif

} // namespace ejmap
