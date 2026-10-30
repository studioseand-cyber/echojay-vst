#pragma once
// =============================================================================================
//  EJLiveBuffer.h - WHAT HAS PLAYED, KEPT ROLLING, SO AI MATCH NEEDS NO SEPARATE CAPTURE
//  (Sean's ruling, 29 Oct 2026; C2 §R4-3; Kathy's six requirements, DESIGN_MATCH_TAB rev 11.1)
// =============================================================================================
//  His words:
//
//      *"AI MATCH must work with the "Live signal" slot, using what has played, with no separate
//      capture needed. Build a live accumulator: while the transport plays, keep the bus's hop
//      series (and each Link's, plus the rack-input tap, the same as a capture) in a rolling
//      buffer, capped ... reset on a large jump or a new song, and skip silence."*
//
//  ---- WHY THIS IS NOT A MeterEngine, WHICH ALREADY HAS A HOP SERIES ------------------------
//
//  MeterEngine keeps hops in a vector reserved in prepare() and NEVER grown on the audio thread,
//  and at its 12,000-hop ceiling it STOPS APPENDING and says so (hopFramesTruncated). That is the
//  right design for a capture, whose length is bounded by the user letting go of the button, and
//  its own comment says why it does not decimate instead: "halving 700 KB on the audio thread is
//  a spike nobody asked for".
//
//  A rolling buffer has to drop the OLDEST hop, for ever, which is the one thing that design
//  refuses to do. So this is a ring on the MESSAGE THREAD: a tick reads the engine's hops, copies
//  the ones it has not taken yet, and the audio thread is untouched. The engine is reset by the
//  same tick when it nears its ceiling, and `takenHops` is how the two stay in step across that
//  reset without losing or double-counting a hop.
//
//  ---- EVERY RULE HERE IS ONE OF KATHY'S SIX, AND IS PURE SO IT CAN BE LEGGED ----------------
//
//    1  never the reference as the mix     `paused` - the caller stops feeding while the
//                                          reference is audible, and the state says so
//    2  a Go marker                        `markAt` - hops before it are stale, counted not
//                                          returned, so "after" means after
//    3  the press freezes it               `snapshot()` returns BY VALUE
//    4  what was played, as played         hops in PLAY ORDER, stops excluded, repeats recorded
//    5  partial results                    toneReady / dynamicsReady separately, with `missing`
//    6  one hop grid for every channel     `hopCount` per channel, and a channel outside the
//                                          tolerance is UNKNOWN rather than aligned by force
// =============================================================================================

#include <JuceHeader.h>
#include "MeterEngine.h"

namespace echojay
{

/** Hops a second, which is MeterEngine's own grid (100 ms). Named once so nothing recomputes it. */
inline constexpr double kLiveHopsPerSecond = 10.0;

/** C7's `match.liveBufferMaxS`, and A12's cap is where the number comes from. Memory only: at
    sixteen floats a hop this is about 384 KB a channel, which is the cost Sean accepted. */
inline constexpr float kLiveBufferMaxSDefault = 600.0f;

/** Kathy's minimums, from C7's tunables. Tone is ready long before dynamics are, which is the
    whole of requirement 5: at 40 s the bands are real and the dynamics are not. */
inline constexpr float kLiveMinToneSDefault     = 30.0f;
inline constexpr float kLiveMinDynamicsSDefault = 60.0f;

/** A replayed passage, described rather than merged (requirement 4). The host timeline DESCRIBES
    the repeat; it never keys the buffer, because a key by position makes a loop overwrite itself
    and lose the weight of what he actually heard. */
struct LiveRepeat
{
    float fromS = 0.0f, toS = 0.0f;
    int   times = 0;
};

/** What the page reads to decide whether to offer AI MATCH, and what to say when it cannot. */
struct LiveBufferState
{
    bool  running        = false;   ///< the transport is rolling and hops are being kept
    bool  pausedAudition = false;   ///< the REFERENCE is audible, so nothing is being kept
    float playedS        = 0.0f;    ///< audio actually kept: stops excluded, silence skipped
    float uniqueS        = 0.0f;    ///< of that, distinct material (a loop counted once)
    float staleS         = 0.0f;    ///< dropped by a marker: a plan's Go, or a chain change
    bool  toneReady      = false;
    bool  dynamicsReady  = false;
    juce::StringArray missing;      ///< plain names of what is not ready yet, for her message
    juce::String      why;          ///< why nothing is being kept, when nothing is
};

/** The rolling series for ONE channel, with the bookkeeping that makes it honest.

    MESSAGE THREAD ONLY. Nothing here is called from processBlock: the audio thread's only part in
    this is feeding a MeterEngine, which it already does. */
class LiveHopRing
{
public:
    void setMaxSeconds (float s) noexcept
    {
        maxHops_ = (size_t) juce::jmax (10.0, (double) s * kLiveHopsPerSecond);
        trim();
    }

    /** Take the hops this ring has not seen yet from an engine's series.

        `engineHops` is the engine's whole vector and `takenHops_` is how many of it are already
        here - so a reset of the engine (which the caller does near its ceiling) is handled by
        `noteEngineReset`, and nothing is lost or counted twice across it.

        `positionS` is the host timeline position of the NEWEST hop, or a negative number when the
        host publishes none; it is used only to describe repeats. */
    void take (const std::vector<MeterEngine::HopFrame>& engineHops, double positionS)
    {
        if (engineHops.size() <= takenHops_) { takenHops_ = juce::jmin (takenHops_, engineHops.size()); return; }
        const size_t first = takenHops_;
        const size_t n     = engineHops.size() - first;
        for (size_t i = 0; i < n; ++i)
        {
            hops_.push_back (engineHops[first + i]);
            // The position of each hop, back-dated from the newest one at the grid's own rate.
            const double back = (double) (n - 1 - i) / kLiveHopsPerSecond;
            posS_.push_back (positionS >= 0.0 ? (float) (positionS - back) : -1.0f);
        }
        takenHops_ = engineHops.size();
        trim();
    }

    /** The engine was reset (it was near its ceiling), so the next take starts from its first hop
        again. The ring keeps everything it already has. */
    void noteEngineReset() noexcept { takenHops_ = 0; }

    /** A marker: everything in the ring now is BEFORE it (requirement 2). The hops are kept so
        `staleS` can say how much was dropped, and `snapshot` returns only what came after. */
    void mark (const juce::String& reason)
    {
        markAt_ = hops_.size();
        markReason_ = reason;
    }

    void clear() noexcept
    {
        hops_.clear(); posS_.clear();
        takenHops_ = 0; markAt_ = 0; markReason_.clear();
    }

    /** The hops AFTER the marker, by value - which is the freeze (requirement 3). */
    std::vector<MeterEngine::HopFrame> snapshot() const
    {
        if (markAt_ >= hops_.size()) return {};
        return std::vector<MeterEngine::HopFrame> (hops_.begin() + (long) markAt_, hops_.end());
    }

    float playedS() const noexcept { return (float) ((double) freshCount() / kLiveHopsPerSecond); }
    float staleS()  const noexcept { return (float) ((double) juce::jmin (markAt_, hops_.size()) / kLiveHopsPerSecond); }
    size_t hopCount() const noexcept { return freshCount(); }
    const juce::String& markReason() const noexcept { return markReason_; }

    /** DISTINCT material, and the repeats that describe the rest (requirement 4).

        Two hops are the same material when their host positions are within half a hop. A host that
        publishes no position cannot tell a loop from new audio, and then every hop is unique -
        which is the honest answer, not a guess. */
    float uniqueS (std::vector<LiveRepeat>* repeatsOut = nullptr) const
    {
        const size_t from = juce::jmin (markAt_, hops_.size());
        if (from >= hops_.size()) return 0.0f;
        const double tol = 0.5 / kLiveHopsPerSecond;
        std::vector<float> seen;
        std::vector<LiveRepeat> reps;
        size_t unique = 0;
        for (size_t i = from; i < posS_.size() && i < hops_.size(); ++i)
        {
            const float p = posS_[i];
            if (p < 0.0f) { ++unique; continue; }                 // no position: cannot be a repeat
            bool found = false;
            for (auto& s : seen)
                if (std::abs ((double) s - (double) p) <= tol) { found = true; break; }
            if (! found) { seen.push_back (p); ++unique; continue; }
            // a repeat: extend the open one, or open a new one
            if (! reps.empty() && std::abs ((double) reps.back().toS - (double) p) <= tol * 3.0)
            { reps.back().toS = p; }
            else
            { LiveRepeat r; r.fromS = p; r.toS = p; r.times = 2; reps.push_back (r); }
        }
        if (repeatsOut != nullptr) *repeatsOut = reps;
        return (float) ((double) unique / kLiveHopsPerSecond);
    }

private:
    size_t freshCount() const noexcept
    {
        const size_t from = juce::jmin (markAt_, hops_.size());
        return hops_.size() - from;
    }
    void trim()
    {
        if (hops_.size() <= maxHops_) return;
        const size_t drop = hops_.size() - maxHops_;
        hops_.erase (hops_.begin(), hops_.begin() + (long) drop);
        posS_.erase (posS_.begin(), posS_.begin() + (long) juce::jmin (drop, posS_.size()));
        markAt_ = markAt_ > drop ? markAt_ - drop : 0;
    }

    std::vector<MeterEngine::HopFrame> hops_;
    std::vector<float> posS_;
    size_t takenHops_ = 0;
    size_t markAt_    = 0;
    size_t maxHops_   = (size_t) (kLiveBufferMaxSDefault * kLiveHopsPerSecond);
    juce::String markReason_;
};

/** The readiness decision, pure, so her page's message and this buffer cannot disagree.

    AT 40 SECONDS THE BANDS ARE REAL AND THE DYNAMICS ARE NOT, which is requirement 5. The two are
    separate answers and `missing` names what is not ready in words she can show. */
inline LiveBufferState liveBufferState (bool transportRunning,
                                        bool referenceAudible,
                                        float playedS,
                                        float uniqueS,
                                        float staleS,
                                        float minToneS = kLiveMinToneSDefault,
                                        float minDynamicsS = kLiveMinDynamicsSDefault)
{
    LiveBufferState s;
    s.pausedAudition = referenceAudible;
    s.running  = transportRunning && ! referenceAudible;
    s.playedS  = juce::jmax (0.0f, playedS);
    s.uniqueS  = juce::jmax (0.0f, uniqueS);
    s.staleS   = juce::jmax (0.0f, staleS);
    s.toneReady     = s.playedS >= minToneS;
    s.dynamicsReady = s.playedS >= minDynamicsS;
    if (! s.toneReady)     s.missing.add ("tone");
    if (! s.dynamicsReady) s.missing.add ("dynamics and loudness");
    if (referenceAudible)
        s.why = "the reference is audible, so nothing is being kept - what plays now is not the mix";
    else if (! transportRunning)
        s.why = "the transport is stopped";
    else if (s.playedS <= 0.0f)
        s.why = "nothing above the silence floor has played yet";
    return s;
}

/** The sentence when too little has played. His wording, and the server's when there is a feed. */
inline juce::String liveTooShortLine (float minToneS = kLiveMinToneSDefault)
{
    return "Play at least " + juce::String ((int) minToneS)
         + " seconds of the song, then press AI MATCH again.";
}

} // namespace echojay
