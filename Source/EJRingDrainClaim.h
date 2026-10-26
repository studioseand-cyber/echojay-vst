#pragma once

#include <atomic>

/** WHO IS DRAINING THE VALUE RING RIGHT NOW (his condition, 26 Oct 2026).

    The value ring is SINGLE-CONSUMER by construction: `drain` advances one read index and hands the
    frames to one caller. Until today its only consumer was the audio thread, inside processBlock -
    which is right while audio is running and useless when it is not: with the transport stopped,
    Logic does not call processBlock, nothing drains, and a knob moved in the V2's live view sits in
    the ring unapplied. That is the fault of 16:35 on 26 Oct, and it is older than the real editor.

    So the Link's timer drains it too. His condition on that, and he is right:

        *"'processBlock hasn't drained for 200 ms' is a time check, not exclusion. If the transport
         starts while the timer is draining, you have two consumers on a single-consumer ring. Make
         it a real single-consumer guard (an atomic try-claim: the audio thread never blocks, and if
         the timer holds it, the audio thread skips draining that block)."*

    Hence two separate things, and the difference matters:

      * THE CLAIM (this file) is CORRECTNESS. One atomic compare-exchange decides who may drain. The
        audio thread NEVER blocks and never waits: if the timer holds the claim, that block simply
        does not drain, and the frames are still there a moment later - a value one buffer late is
        the cost, where two consumers on one read index would be lost or duplicated frames.
      * THE IDLE TEST (`timerMayDrain`) is POLICY: the timer only wants the ring when the audio
        thread is not servicing it, so a move still lands on the same buffer whenever audio runs.
        Nothing's correctness rests on this clock - if it is wrong, the claim still allows only one
        consumer and the worst case is a value applied a tick later.

    Lock-free, allocation-free, and safe on the audio thread. */
namespace echojay
{

/** The one-consumer claim. Zero means free; the holder sets it and clears it on the way out. */
using RingDrainFlag = std::atomic<int>;

/** RAII, non-blocking. `held()` is false when somebody else has it: the caller then does nothing at
    all this pass rather than waiting. */
class RingDrainClaim
{
public:
    explicit RingDrainClaim (RingDrainFlag& flag) noexcept : flag_ (flag)
    {
        int expected = 0;
        held_ = flag_.compare_exchange_strong (expected, 1,
                                               std::memory_order_acquire, std::memory_order_relaxed);
    }
    ~RingDrainClaim() noexcept { if (held_) flag_.store (0, std::memory_order_release); }

    bool held() const noexcept { return held_; }

    RingDrainClaim (const RingDrainClaim&) = delete;
    RingDrainClaim& operator= (const RingDrainClaim&) = delete;

private:
    RingDrainFlag& flag_;
    bool held_ = false;
};

/** The timer's POLICY: drain only when the audio thread has not drained recently. A Link that has
    never processed a block (lastAudioDrainMs == 0) counts as idle - that is the transport-stopped
    case at its most literal, a session opened and a knob moved before anything was played. */
inline constexpr double kAudioIdleForDrainMs = 200.0;

inline bool timerMayDrain (double nowMs, double lastAudioDrainMs,
                           double idleMs = kAudioIdleForDrainMs) noexcept
{
    if (lastAudioDrainMs <= 0.0) return true;
    return (nowMs - lastAudioDrainMs) >= idleMs;
}

} // namespace echojay
