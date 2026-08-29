/*
    EedKeyFeed.h  —  the one place a built-in device can read the session's
    DETECTED KEY (PITCH_CORRECTION_SPEC.md §6).

    WHY THIS EXISTS AT ALL. The precedence walk that decides which key source
    wins - newest capture, then a bus Link, then this plugin on a music bus,
    then a channel Link, then the local chain - already exists exactly once, in
    EchoJayProcessor::collectKeySources(), and the [DETECTED KEY] feed block,
    the Meters KEY panel and this feed all read it. A device must NOT
    re-implement that walk; two rankings that can disagree is precisely the bug
    the shared collector was written to prevent.

    So the PROCESSOR publishes its resolved primary here from its own 1 Hz
    timer (the editor's 2 Hz refresh republishes the same walk while a window
    is open), and devices READ it. It was the editor's job alone once, and
    that was a bug: with the window closed nothing published, so the key froze
    at its last value - a stale key applied with confidence, the exact failure
    the chromatic fallback below exists to prevent. The walk stays in one
    place and the device stays self-contained - no back pointer to the editor,
    no edit to ChainHost, consistent with the registry rule that a device
    integrates through its own files.

    SCOPE HONESTY. This is process-wide, like DashPoll's poller. Several plugin
    instances each publish their own resolved primary, so the last writer wins.
    That is sound for the fact being carried - a bus reading is the key of the
    MUSIC, which is a property of the session and not of one channel, and the
    precedence walk prefers bus sources from every instance - but it is a real
    limitation and not a claim that per-instance keys are supported.

    THREADING. One relaxed atomic generation counter guards a plain struct:
    the publisher bumps it after writing, a reader re-checks it after copying
    and retries once. Readers are on the audio thread and must never block, and
    a key that is one 2 Hz tick stale is worth nothing to complain about.
*/

#pragma once

#include <atomic>
#include <cstring>
#include <cstdint>

namespace echojay
{

struct DetectedKeyFact
{
    bool     valid      = false;   // false = no usable source at all
    int      root       = 0;       // 0..11, C..B
    bool     minor      = false;
    float    confidence = 0.0f;
    float    tuningHz   = 440.0f;  // detected reference pitch
    uint32_t ageMs      = 0;

    // Attribution, so the UI can say WHERE the key came from. A wrong reading
    // is only diagnosable if the user can see which source produced it.
    char     sourceName[48] = {};
    bool     fromBus    = false;   // a bus-grade source (bus Link / self on bus)

    // CIRCULARITY GUARD (29 Aug 2026 ruling): a corrector must never derive
    // its tuning grid from the signal it is correcting - a flat singer would
    // define a flat grid and correction becomes a no-op in the mean. The
    // publisher stamps WHO it is and whether the primary source was its own
    // channel's audio (self analysis, a capture of this channel, the local
    // chain); a reader hosted by the SAME instance must then treat the
    // tuning as unmeasured and fall back to 440, never to the channel.
    uint64_t publisherId = 0;      // the publishing instance (0 = unknown)
    bool     selfDerived = false;  // primary came from the publisher's own channel

    // The confidence gate of spec §6. Below it the key is NOT applied and the
    // device falls back to chromatic - never to the last known key, because a
    // stale key is applied with total confidence and can force a note that is
    // wrong for the song. Chromatic still tunes and cannot do that. Measured:
    // correcting to a wrong key pushed a take from 13 cents off the nearest
    // note to 29, i.e. worse than not correcting.
    static constexpr float kConfidenceGate = 0.50f;

    bool usable() const noexcept { return valid && confidence >= kConfidenceGate; }
};

// Implemented by chain-hosted devices that consume the feed and need to know
// which instance HOSTS them, so they can recognise a fact derived from their
// own channel (see publisherId/selfDerived above). ChainHost calls this on
// every builtin it creates; devices that don't care don't implement it.
struct KeyFeedConsumer
{
    virtual ~KeyFeedConsumer() = default;
    virtual void setKeyFeedSelfId (uint64_t id) = 0;
};

class KeyFeed
{
public:
    static KeyFeed& instance() noexcept
    {
        static KeyFeed f;
        return f;
    }

    // Message thread (the processor's 1 Hz timer; the editor's 2 Hz refresh).
    void publish (const DetectedKeyFact& f) noexcept
    {
        gen_.fetch_add (1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        fact_ = f;
        std::atomic_thread_fence (std::memory_order_release);
        gen_.fetch_add (1, std::memory_order_relaxed);
    }

    // Any thread, including audio. Never blocks.
    DetectedKeyFact get() const noexcept
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const uint32_t before = gen_.load (std::memory_order_relaxed);
            if (before & 1u) continue;                    // a write is in flight
            std::atomic_thread_fence (std::memory_order_acquire);
            DetectedKeyFact out = fact_;
            std::atomic_thread_fence (std::memory_order_acquire);
            if (gen_.load (std::memory_order_relaxed) == before) return out;
        }
        return {};
    }

    // Nothing has ever published: distinguishes "no key" from "never asked",
    // which is what decides whether a precondition is worth raising.
    bool everPublished() const noexcept { return gen_.load (std::memory_order_relaxed) != 0; }

private:
    KeyFeed() = default;
    std::atomic<uint32_t> gen_ { 0 };
    DetectedKeyFact       fact_;
};

} // namespace echojay
