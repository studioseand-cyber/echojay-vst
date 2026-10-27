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
// expectedGuestView: the NSView that JUCE gave THIS editor, when we can identify it (an AU's
// editor holds it in a juce::NSViewComponent). Non-null means the adopt is a fact rather than a
// guess: that view and no other is put in the container, and no other view is adopted from the
// peer. Null (a VST3's editor attaches no NSViewComponent) falls back to the newest-stray search,
// which is what showed Apple Delay's panel under a Tube-Tech CL 1B's title on 26 Oct 2026.
extern "C" bool NativeClip2_attach(void* peerHandle, int x, int y, int w, int h, bool doLog,
                                   int desiredW, int desiredH, void* expectedGuestView);
extern "C" bool NativeClip2_getPluginSize(void* peerHandle, int* outW, int* outH);
extern "C" bool NativeClip2_getPluginSizeVerbose(void* peerHandle, int* outW, int* outH,
                                                 char* desc, int descLen);
extern "C" void NativeClip2_detach(void* peerHandle);
/** Move a guest view into this bundle's parking view - one addSubview, nothing removed first - so a
    lent editor can be KEPT while nobody shows it (his ruling of 26 Oct 21:08). Called by the LENDER,
    in the lender's own image. */
extern "C" bool NativeClip2_parkGuestView(void* guestView);
/** Hide or show the clip container WITHOUT moving anything inside it (round 2, 26 Oct 2026): a lent
    editor that is coming back must never see its view hop to a nil window. */
extern "C" void NativeClip2_setHidden(void* peerHandle, bool hidden);
/** Remove the container only when it is EMPTY - i.e. after the editor and its view have already
    been destroyed by their owner. Refuses, loudly, if a view is still inside. */
extern "C" void NativeClip2_detachEmptyContainer(void* peerHandle);
// The READ-ONLY SHIELD (24 Oct 2026): a transparent NSView of ours, kept topmost inside the clip
// container, that swallows every gesture aimed at the hosted plugin's own view and draws one line
// along its top edge. JUCE's setInterceptsMouseClicks cannot do this - a guest AU's NSView is never
// under JUCE's mouse handling - which is why it is here. See Source/EJLiveViewInput.h for WHEN.
extern "C" bool NativeClip2_setShield(void* peerHandle, bool on, const char* caption);

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
                       int desiredW = 0, int desiredH = 0, void* expectedGuestView = nullptr)
    {
        if (!comp) return false;
        auto* top = comp->getTopLevelComponent();
        if (!top) return false;
        auto* handle = top->getWindowHandle();
        if (!handle) return false;
        auto r = top->getLocalArea(comp, areaInComp);
        return NativeClip2_attach(handle, r.getX(), r.getY(), r.getWidth(), r.getHeight(), doLog,
                                  desiredW, desiredH, expectedGuestView);
    }

    // The hosted plugin view's ACTUAL native frame size. Degenerate sizes
    // (e.g. 1x1) mean "not ready yet" — retry briefly.
    inline bool getPluginViewSize(juce::Component* comp, int& w, int& h)
    {
        w = h = 0;
        auto* handle = windowHandleFor(comp);
        return handle != nullptr && NativeClip2_getPluginSize(handle, &w, &h);
    }

    /** The lender takes its guest view back out of a borrower's window, in one move. */
    inline bool parkGuestView (void* guestView) { return NativeClip2_parkGuestView (guestView); }

    /** Hide the clip container where it stands, or show it again. Nothing inside it moves. */
    inline void setHidden(juce::Component* comp, bool hidden)
    {
        if (auto* handle = windowHandleFor(comp)) NativeClip2_setHidden(handle, hidden);
    }

    /** Remove our container once its guest has gone with the editor that owned it. */
    inline void detachEmptyContainer(juce::Component* comp)
    {
        if (auto* handle = windowHandleFor(comp)) NativeClip2_detachEmptyContainer(handle);
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

    // Put the read-only shield up (or take it down). Call after every attach: attach reparents the
    // plugin's view, and the shield only blocks what it is drawn above.
    inline bool setShield(juce::Component* comp, bool on, const juce::String& caption = {})
    {
        auto* handle = windowHandleFor(comp);
        return handle != nullptr
            && NativeClip2_setShield(handle, on, caption.toRawUTF8());
    }

    // Remove the container (call AFTER the hosted editor is destroyed).
    inline void detach(juce::Component* comp)
    {
        if (auto* handle = windowHandleFor(comp))
            NativeClip2_detach(handle);
    }
}
