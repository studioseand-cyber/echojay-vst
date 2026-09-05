#pragma once

// ===========================================================================
// DO NOT DIAL: EchoJay suggests every value and writes none (5 Sep 2026).
//
// The user hand-dials from the card. This is the mode the server already
// implements as dialWritesBlocked, and it is a mode about WRITING, not about
// suggesting: the card must still carry every value, or the feature removes
// the thing it exists to provide.
//
// WHY A PROCESS-WIDE FLAG RATHER THAN A THREADED ARGUMENT. Three different
// code paths write a value, and they do not share a call stack:
//
//   EchoJayParamApply.h  applyOne          every mapped third-party plugin
//   EedDeviceProcessor   applyParams       every EchoJay built-in device
//   ChainHost            setSlotWet        EchoJay's own per-slot blend
//
// Threading a bool to all three means three plumbings that can disagree, and
// the failure that produces is the worst version of this feature: third-party
// plugins correctly refusing to move while EchoJay's own EQ dials itself. One
// authority, read by all three, cannot drift.
//
// IT IS NOT A WRITE GATE FOR EVERYTHING. Deliberately NOT consulted by:
//   ChainHost::applyRestoredParams   restores what the USER saved with the
//                                    session; blocking it corrupts a reload
//   setSlotSettings / setSlotStructuredSettings
//                                    these FILL THE CARD, which is the whole
//                                    point of the mode
//   the plugin load path             Build still builds: plugins load at
//                                    their defaults and the values go on the
//                                    card. This blocks writing values, not
//                                    adding slots.
//
// Header-only and dependency-free on purpose, so the gate can drive the real
// predicate rather than a copy, and so EchoJayParamApply.h keeps compiling
// without EchoJayAPI.h.
// ===========================================================================

#include <atomic>

namespace echojay
{

// Message thread writes it, the message thread reads it. Atomic anyway: the
// apply pipeline is called from load callbacks and timers, and a torn read of
// a bool is not worth reasoning about.
inline std::atomic<bool>& dialWritesBlockedFlag() noexcept
{
    static std::atomic<bool> f { false };
    return f;
}

/** True when EchoJay must suggest values and write none. */
inline bool dialWritesBlocked() noexcept
{
    return dialWritesBlockedFlag().load (std::memory_order_relaxed);
}

/** Mirrored here from the persisted setting; EchoJayAPI::setDialWritesBlocked
    is the only writer that matters, plus loadSettings at startup. */
inline void setDialWritesBlocked (bool on) noexcept
{
    dialWritesBlockedFlag().store (on, std::memory_order_relaxed);
}

} // namespace echojay
