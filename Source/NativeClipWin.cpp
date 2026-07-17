// NativeClipWin.cpp — non-Apple implementation of the NativeClip surface.
//
// The Apple build gets these four symbols from NativeClip.mm (Obj-C++/Cocoa).
// That file can never compile under MSVC, so it is Apple-only in CMake and this
// file supplies the same extern "C" surface everywhere else. Deliberately does
// NOT include JuceHeader, matching NativeClip.mm (avoids Component ambiguity).
//
// Split of responsibilities, because the two halves differ in what they can
// honour off macOS:
//
//   EchoJay_NSLog        — REAL implementation. It is the diagnostics bridge
//                          used ~70x across the codebase (EJChat:, EJLinkState:,
//                          EJCodec: …) and has nothing Cocoa-specific about it.
//                          NSLog lands in Console.app; OutputDebugString lands
//                          in DebugView / the VS output window, which is the
//                          direct Windows analogue.
//
//   NativeClip2_attach / _getPluginSize / _detach
//                        — NO-OP STUBS. These reparent a hosted plugin's NSView
//                          into a container whose frame is locked to our layout
//                          so masksToBounds can trim overflow. That exists
//                          because JUCE attaches a hosted AU's NSView straight
//                          to the window peer, escaping JUCE's own clipping.
//                          The Windows equivalent (container HWND + reparenting
//                          the plugin's child HWND, with DPI negotiation) is a
//                          real port, not a translation — out of scope for beta.
//
// Stub semantics are chosen to match what the Cocoa side returns when it finds
// no plugin view, which the callers already treat as "not ready / can't inline"
// and handle: layoutInline() falls back to the JUCE-reported editor size, and
// the poll timer falls back to the floating pop-out window. See the report/
// commit message for the resulting Windows behaviour.

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <cstdio>
#endif

// Per-target log tag, mirroring NativeClip.mm — both plugins can be loaded in
// one host process and their diagnostics must stay distinguishable.
#ifndef NATIVECLIP_LOG_TAG
#define NATIVECLIP_LOG_TAG "NativeClip2"
#endif

extern "C"
{

// Diagnostics bridge. NSLog on macOS; OutputDebugString here. Callers pass an
// already-formatted UTF-8 line, so there is no format string to interpret.
void EchoJay_NSLog(const char* msg)
{
    const char* text = (msg != nullptr) ? msg : "(null)";
#if defined(_WIN32)
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
#else
    std::fputs(text, stderr);
    std::fputc('\n', stderr);
#endif
}

// No clipping container off macOS. Returning false means "not attached"; both
// call sites already discard the result (they re-assert every layout pass), so
// this is inert rather than an error path.
bool NativeClip2_attach(void* /*peerHandle*/, int /*x*/, int /*y*/, int /*w*/, int /*h*/,
                        bool /*doLog*/, int /*desiredW*/, int /*desiredH*/)
{
    return false;
}

// "No plugin view found" — identical to the Cocoa result before the hosted view
// exists. Callers must see 0/0 + false so layoutInline() takes its JUCE-size
// fallback (realW/realH stay 0) instead of laying out against stale garbage.
bool NativeClip2_getPluginSize(void* /*peerHandle*/, int* outW, int* outH)
{
    if (outW != nullptr) *outW = 0;
    if (outH != nullptr) *outH = 0;
    return false;
}

// Nothing was ever parented into a container, so there is nothing to unparent.
// JUCE's own teardown still destroys the hosted editor normally.
void NativeClip2_detach(void* /*peerHandle*/)
{
}

} // extern "C"
