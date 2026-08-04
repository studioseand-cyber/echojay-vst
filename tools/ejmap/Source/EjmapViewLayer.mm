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

juce::String ensurePeerLayerBacked (juce::Component& topLevel)
{
    NSView* peer = peerViewOf (topLevel);
    if (peer == nil)
        return "no peer yet: nothing done (the window must exist first)";

    if ([peer wantsLayer])
        return "peer already layer-backed: nothing done";

    [peer setWantsLayer: YES];

    // STATE THE OBSERVATION. setWantsLayer is a request; AppKit decides when
    // the layer appears. Reporting "done" without reading it back would be the
    // class this project keeps filing.
    const bool now = [peer wantsLayer];
    const bool hasLayer = ([peer layer] != nil);
    return juce::String ("peer setWantsLayer:YES -> wantsLayer=")
             + (now ? "YES" : "no") + ", layer=" + (hasLayer ? "yes" : "nil");
}

#else

juce::String describeViewTree (juce::Component&)      { return {}; }
juce::String ensurePeerLayerBacked (juce::Component&) { return "not macOS: nothing to do"; }

#endif

} // namespace ejmap
