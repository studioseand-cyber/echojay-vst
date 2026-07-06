#pragma once
#include <JuceHeader.h>

// C functions implemented in NativeClip.mm (Obj-C++) — inline plugin-editor
// containment. The display area is a dedicated container NSView whose frame
// is LOCKED to our layout (never sized from the plugin); masksToBounds lives
// on that container and the plugin view is reparented inside it at native
// size. See the .mm for details. Diagnostics are NSLog'd with the prefix
// "NativeClip2:" when the log flag is set.
extern "C" bool NativeClip2_attach(void* peerHandle, int x, int y, int w, int h, bool doLog);
extern "C" bool NativeClip2_getPluginSize(void* peerHandle, int* outW, int* outH);
extern "C" void NativeClip2_detach(void* peerHandle);

// Unified-log bridge (NSLog) so C++ diagnostics show up in Console.app
extern "C" void EchoJay_NSLog(const char* msg);

namespace NativeClip
{
    inline void* windowHandleFor(juce::Component* comp)
    {
        if (!comp) return nullptr;
        auto* top = comp->getTopLevelComponent();
        return top ? top->getWindowHandle() : nullptr;
    }

    // Ensure the clipping container covers `areaInComp` (given in comp's own
    // coordinate space), reparent the hosted plugin view into it and centre
    // it. Call again after any layout change — it only ever re-asserts OUR
    // frame; it never resizes the container to the plugin.
    inline bool attach(juce::Component* comp, juce::Rectangle<int> areaInComp, bool doLog)
    {
        if (!comp) return false;
        auto* top = comp->getTopLevelComponent();
        if (!top) return false;
        auto* handle = top->getWindowHandle();
        if (!handle) return false;
        auto r = top->getLocalArea(comp, areaInComp);
        return NativeClip2_attach(handle, r.getX(), r.getY(), r.getWidth(), r.getHeight(), doLog);
    }

    // The hosted plugin view's ACTUAL native frame size. Degenerate sizes
    // (e.g. 1x1) mean "not ready yet" — retry briefly.
    inline bool getPluginViewSize(juce::Component* comp, int& w, int& h)
    {
        w = h = 0;
        auto* handle = windowHandleFor(comp);
        return handle != nullptr && NativeClip2_getPluginSize(handle, &w, &h);
    }

    // Remove the container (call AFTER the hosted editor is destroyed).
    inline void detach(juce::Component* comp)
    {
        if (auto* handle = windowHandleFor(comp))
            NativeClip2_detach(handle);
    }
}
