#pragma once
#include <JuceHeader.h>

// C functions implemented in NativeClip.mm (Obj-C++) — inline plugin-editor
// containment. The display area is a dedicated container NSView whose frame
// is LOCKED to our layout (never sized from the plugin); masksToBounds lives
// on that container and the plugin view is reparented inside it at native
// size. See the .mm for details. Diagnostics are NSLog'd with the prefix
// "NativeClip2:" when the log flag is set.
// desiredW/desiredH: the JUCE editor's reported preferred size. Out-of-process
// view proxies (AUv2ContainerView) may wait for the HOST to impose a size —
// when the found view is still degenerate and a sane desired size is given,
// it is imposed (logged before/after). 0/0 disables imposition.
extern "C" bool NativeClip2_attach(void* peerHandle, int x, int y, int w, int h, bool doLog,
                                   int desiredW, int desiredH);
extern "C" bool NativeClip2_getPluginSize(void* peerHandle, int* outW, int* outH);
extern "C" bool NativeClip2_getPluginSizeVerbose(void* peerHandle, int* outW, int* outH,
                                                 char* desc, int descLen);
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
    inline bool attach(juce::Component* comp, juce::Rectangle<int> areaInComp, bool doLog,
                       int desiredW = 0, int desiredH = 0)
    {
        if (!comp) return false;
        auto* top = comp->getTopLevelComponent();
        if (!top) return false;
        auto* handle = top->getWindowHandle();
        if (!handle) return false;
        auto r = top->getLocalArea(comp, areaInComp);
        return NativeClip2_attach(handle, r.getX(), r.getY(), r.getWidth(), r.getHeight(), doLog,
                                  desiredW, desiredH);
    }

    // The hosted plugin view's ACTUAL native frame size. Degenerate sizes
    // (e.g. 1x1) mean "not ready yet" — retry briefly.
    inline bool getPluginViewSize(juce::Component* comp, int& w, int& h)
    {
        w = h = 0;
        auto* handle = windowHandleFor(comp);
        return handle != nullptr && NativeClip2_getPluginSize(handle, &w, &h);
    }

    // VERBOSE variant for the settle-verdict log (2 Sep 2026): names WHICH
    // NSView the walk measured (class + frame), the container's own frame,
    // and whether the view came from inside the container or the peer-level
    // stray search — the three facts that decide whether the poll measured
    // the plugin or the pane it was added to.
    inline bool getPluginViewSizeVerbose(juce::Component* comp, int& w, int& h,
                                         juce::String& desc)
    {
        w = h = 0;
        char buf[256] = { 0 };
        auto* handle = windowHandleFor(comp);
        const bool ok = handle != nullptr
            && NativeClip2_getPluginSizeVerbose(handle, &w, &h, buf, (int) sizeof(buf));
        desc = juce::String::fromUTF8(buf);
        return ok;
    }

    // Remove the container (call AFTER the hosted editor is destroyed).
    inline void detach(juce::Component* comp)
    {
        if (auto* handle = windowHandleFor(comp))
            NativeClip2_detach(handle);
    }
}
