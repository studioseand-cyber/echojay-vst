#pragma once
// =============================================================================================
//  EJShadowView - stage B of the muted shadow: the V2's own copy of the slot in view
// =============================================================================================
//  Sean's design, 20 Oct, and his go of 14:19 on 21 Oct:
//
//    *"B: the V2 hosts a copy of the slot in view, fed from your tap ring, output discarded, so its
//    own meters and GR move like the real one."*
//    *"The Link stays the only real processor; what's heard never changes."*
//
//  ---- THIS MACHINERY ALREADY EXISTED ONCE, AND WHY IT WAS RETIRED -------------------------------
//
//  The audition (editInst_ / editBuf_ / editLock_) is the same shape: host a copy, consume a Link's
//  ring into a scratch buffer, run the copy, discard the result. Its intake was removed on 11 Oct
//  for a reason that is worth keeping in front of us, in PluginProcessor's own words:
//
//    *"The ring taps POST-RACK, and since stage 1 the Link's own rack is no longer bypassed while
//    addressed - so this consumed a channel that had already been processed and processed it AGAIN.
//    Anything that MEASURED on that copy was measuring double processing."*
//
//  THE SHADOW'S RING IS THE CURE FOR EXACTLY THAT: it carries the signal at the INPUT OF ONE SLOT,
//  taken after that slot's pre-trim - "the signal the plugin actually hears" (ChainHost's 28 Sep
//  rule, quoted in EJShadowTap.h). So the copy hears what the real plugin hears, and its meters and
//  GR are a measurement of the same input rather than of a second pass over a finished mix.
//
//  ---- WHAT THIS OBJECT WILL NOT DO --------------------------------------------------------------
//
//    - it never writes its output anywhere. `pump` fills a scratch buffer and throws it away; the
//      only thing that leaves here is a reading;
//    - it never writes the slot's state anywhere (stage C's rule, stated here because this is the
//      object that would be tempted): a renderer that holds no state cannot go stale, which is
//      precisely what made the old mirror wrong;
//    - it allocates nothing and locks nothing on the audio thread. The buffer is allocated at
//      prepare, the instance is swapped under a SpinLock the audio thread only ever TRIES, and a
//      host block larger than the buffer skips the block rather than growing it.
#include <JuceHeader.h>
#include <atomic>
#include "LinkShm.h"
#include "EJShadowTap.h"

namespace echojay
{

/** The V2's side of the shadow: one hosted copy, one ring, and three readings.

    Owned by the processor (it outlives any window, like the run does), driven by the editor:
    `start` on a selection, `stop` on leaving, `pump` from processBlock. */
class ShadowView
{
public:
    static constexpr int   kBufFrames   = 8192;   // the audition's capacity, and for its reason
    static constexpr uint32_t kCushion  = 1024;   // ~21 ms at 48 k (the plan's figure)
    static constexpr uint32_t kReseekTrip = 8192; // ~171 ms outer bound before it re-seeks

    // ---- THE MESSAGE THREAD'S HALF ---------------------------------------------------------

    /** Allocate everything the audio thread will need. Called from prepareToPlay. */
    void prepare (double sampleRate, int blockSize)
    {
        juce::ignoreUnused (blockSize);
        hostRate_ = sampleRate;
        buf_.setSize (2, kBufFrames);
        buf_.clear();
    }

    /** Install a prepared copy and the ring it feeds from.

        `inst` must already have had prepareToPlay called with the HOST's rate and block (the
        audition's rule: "the audio thread must never meet a plugin that has not seen
        prepareToPlay"). `ringMap` is a mapped shadow ring, read-write because ringConsume writes
        the read cursor back into the mapping. Returns false and changes nothing when the ring's
        rate disagrees with ours - the plan's rule is COMPARE AND REFUSE, never resample. */
    bool start (std::unique_ptr<juce::AudioProcessor> inst, void* ringMap,
                const juce::String& uid, int slot0, const juce::String& pluginName,
                juce::String& whyNot)
    {
        if (inst == nullptr) { whyNot = "no copy was created"; return false; }
        if (ringMap == nullptr) { whyNot = "that Link is not streaming the slot yet"; return false; }
        auto* hdr = LinkShm::ringHeader (ringMap);
        if (hdr == nullptr || hdr->magic != kLinkMagic)
        { whyNot = "the shadow ring has no header yet"; return false; }
        if (hostRate_ > 0.0 && hdr->sampleRate > 0.0f
            && std::abs ((double) hdr->sampleRate - hostRate_) > 1.0)
        {
            // ONE HOST, ONE ENGINE: a mismatch means something is wrong, not something to paper
            // over. Said in the caller's words, because this is a line he may read.
            whyNot = "that Link is running at " + juce::String ((int) hdr->sampleRate)
                   + " Hz and this instance at " + juce::String ((int) hostRate_) + " Hz";
            return false;
        }
        // Start at the cushion, so the first blocks are not a backlog.
        LinkShm::ringSeekForward (ringMap, kCushion);
        {
            const juce::SpinLock::ScopedLockType sl (lock_);
            inst_ = std::move (inst);
            ring_ = ringMap;
        }
        uid_ = uid; slot0_ = slot0; name_ = pluginName;
        framesPumped_.store (0, std::memory_order_relaxed);
        blocksSkipped_.store (0, std::memory_order_relaxed);
        on_.store (true, std::memory_order_release);
        return true;
    }

    /** Take it down. The audio thread may be inside pump(), so the flag goes first and the
        instance is released under the lock the audio thread only tries - never while it holds it.
        The RING is not unmapped here: its owner (the processor, which mapped the file) does that,
        and only after this has returned. */
    void stop()
    {
        on_.store (false, std::memory_order_release);
        std::unique_ptr<juce::AudioProcessor> dying;
        {
            const juce::SpinLock::ScopedLockType sl (lock_);
            dying = std::move (inst_);
            ring_ = nullptr;
        }
        dying.reset();                      // destroyed on THIS thread, outside the lock
        uid_.clear(); slot0_ = -1; name_.clear();
        grDb_.store (0.0f, std::memory_order_relaxed);
        inDb_.store (-200.0f, std::memory_order_relaxed);
        outDb_.store (-200.0f, std::memory_order_relaxed);
    }

    bool running() const noexcept { return on_.load (std::memory_order_acquire); }
    juce::String uid() const { return uid_; }
    int  slot() const noexcept { return slot0_; }
    juce::String pluginName() const { return name_; }
    juce::int64 framesPumped() const noexcept { return framesPumped_.load (std::memory_order_relaxed); }
    int  blocksSkipped() const noexcept { return blocksSkipped_.load (std::memory_order_relaxed); }

    // ---- THE AUDIO THREAD'S HALF -----------------------------------------------------------

    /** Consume one host block from the ring, run the copy on it, and throw the output away.
        Returns the number of frames consumed, which is what a guard asserts.

        NOTHING HERE IS HEARD. The buffer is local to this object and no caller is given it. */
    int pump (int numSamples)
    {
        if (! on_.load (std::memory_order_acquire) || numSamples <= 0) return 0;
        if (! lock_.tryEnter()) return 0;              // never block the audio thread
        int consumed = 0;
        if (inst_ != nullptr && ring_ != nullptr)
        {
            if (numSamples > buf_.getNumSamples())
            {
                // A host block larger than the capacity SKIPS, rather than allocating here. Counted,
                // so "the shadow is not moving" has an answer other than a guess.
                blocksSkipped_.fetch_add (1, std::memory_order_relaxed);
            }
            else
            {
                // Bounded latency: a backlog past the trip point re-seeks to the cushion, or every
                // missed block would ratchet the shadow later for ever (the audition's lesson).
                auto* hdr = LinkShm::ringHeader (ring_);
                if (hdr != nullptr
                    && LinkShm::loadAcquire (&hdr->writeIdx) - LinkShm::loadRelaxed (&hdr->readIdx) > kReseekTrip)
                    LinkShm::ringSeekForward (ring_, kCushion);
                const uint32_t n = LinkShm::ringConsume (ring_, buf_.getWritePointer (0),
                                                         buf_.getWritePointer (1), numSamples);
                consumed = (int) n;
                // UNDERRUN IS SILENCE, NEVER STALE SAMPLES (the existing rule, kept).
                if ((int) n < numSamples)
                    for (int ch = 0; ch < 2; ++ch)
                        buf_.clear (ch, (int) n, numSamples - (int) n);
                const float inPk = juce::jmax (buf_.getMagnitude (0, 0, numSamples),
                                               buf_.getMagnitude (1, 0, numSamples));
                juce::MidiBuffer noMidi;
                juce::AudioBuffer<float> view (buf_.getArrayOfWritePointers(), 2, 0, numSamples);
                inst_->processBlock (view, noMidi);
                const float outPk = juce::jmax (view.getMagnitude (0, 0, numSamples),
                                                view.getMagnitude (1, 0, numSamples));
                // THE READINGS, and they are the copy's OWN: in, out, and the difference, which is
                // what a compressor's gain reduction looks like from outside the plugin. An EchoJay
                // device would report its own; a third party cannot, and the plan says so - this is
                // the same estimate the loudness loop already reports for a third-party limiter.
                const auto dB = [] (float lin)
                { return lin > 1.0e-6f ? juce::Decibels::gainToDecibels (lin) : -200.0f; };
                const float i = dB (inPk), o = dB (outPk);
                inDb_.store (i, std::memory_order_relaxed);
                outDb_.store (o, std::memory_order_relaxed);
                if (i > -120.0f && o > -120.0f)
                {
                    // smoothed, because a per-block difference on a limiter is a flicker, not a
                    // reading; one pole, no allocation, and the attack is faster than the release
                    // so a hit is seen and the recovery is not jumpy.
                    const float want = juce::jmax (0.0f, i - o);
                    const float cur  = grDb_.load (std::memory_order_relaxed);
                    const float a    = want > cur ? 0.5f : 0.1f;
                    grDb_.store (cur + a * (want - cur), std::memory_order_relaxed);
                }
                framesPumped_.fetch_add (consumed, std::memory_order_relaxed);
            }
        }
        lock_.exit();
        return consumed;
    }

    /** The copy's own readings, for the card. NaN-free: -200 means nothing has been heard. */
    float inDb()  const noexcept { return inDb_.load (std::memory_order_relaxed); }
    float outDb() const noexcept { return outDb_.load (std::memory_order_relaxed); }
    float grDb()  const noexcept { return grDb_.load (std::memory_order_relaxed); }

    /** The hosted copy, for stage C's parameter follow. Message thread only, and the caller must
        hold nothing: the pointer is valid only while `running()` and only on this thread. */
    juce::AudioProcessor* instance() noexcept { return inst_.get(); }
    juce::SpinLock& lock() noexcept { return lock_; }

private:
    juce::SpinLock                           lock_;
    std::unique_ptr<juce::AudioProcessor>    inst_;      // under lock_
    void*                                    ring_ = nullptr;   // under lock_; owned by the caller
    juce::AudioBuffer<float>                 buf_;      // allocated at prepare, audio thread's own
    std::atomic<bool>                        on_ { false };
    std::atomic<float>                       inDb_ { -200.0f }, outDb_ { -200.0f }, grDb_ { 0.0f };
    std::atomic<juce::int64>                 framesPumped_ { 0 };
    std::atomic<int>                         blocksSkipped_ { 0 };
    double                                   hostRate_ = 0.0;
    juce::String                             uid_, name_;
    int                                      slot0_ = -1;
};

}   // namespace echojay
