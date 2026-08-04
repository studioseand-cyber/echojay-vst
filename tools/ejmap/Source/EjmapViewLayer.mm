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

#else

juce::String describeViewTree (juce::Component&)      { return {}; }
juce::String layerBackHostedViews (juce::Component&)  { return "not macOS: nothing to do\n"; }

#endif

} // namespace ejmap
