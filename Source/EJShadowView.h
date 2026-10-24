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
#include "EJPluginCallWindow.h"   // his 08:58 crash, 24 Oct: no render while we are inside a plugin
#include "EJShadowChannels.h"  // his 12:14 crash, 24 Oct: an unfed key bus is a null pointer
#include "EJShadowKeep.h"
#include "EJShadowState.h"  // FOLLOW layer 1, and the repeat-crash marker his ruling asked for      // his 14:27 crash, 24 Oct: a hosted AU is never disposed mid-session

namespace echojay
{

/** ---- THE LIVE VIEW IS OFF IN THE SHIPPED BUILD (his ruling of 16:4x, 23 Oct 2026) ------------

    *"With the 16:33 crash that's two in ten minutes, both on a Link's rack in the V2 ... If the fix
     isn't quick and certain, ship the next build with the shadow view switched OFF (back to the
     "coming soon" placeholder), so I can test the other fixes on a build that doesn't crash. The
     shadow view then comes back in its own round."*

    THE TWO CRASHES WERE ONE DEFECT, and it is fixed below: the copy was handed a two-channel scratch
    buffer whatever it was, and both of his plugins pull more than two channels, so JUCE's input
    callback copied from a null pointer on the render thread (_platform_memmove <-
    renderGetInputCallback <- the plugin's PullInput <- EchoJayProcessor::processBlock, SIGSEGV at
    0x0, AUHostingServiceXPC, 16:33 with WaveShell and 16:41 with UnFairchild).

    SO WHY IS IT STILL OFF? Because "fixed" and "proven" are different words. The fix is guarded by
    the arithmetic (chat_store_guard SH8/SH8b) and the rig will exercise the road, but neither can
    load a PACE or iLok plugin - the standing rule on this machine - so the two plugins that actually
    crashed cannot be tested here by anything but him. A feature that took his session down twice in
    ten minutes does not go back in front of him on the strength of a guard that cannot load it.

    ONE SWITCH, READ IN ONE PLACE (the editor's follow road), so turning it on is one line and
    nothing else changes. Everything else in the feature - the tap, the ring, the refusals, the
    stale-index fix, the frame count - stays in and stays guarded.

    ---- AND IT IS BACK ON, BY HIS RULING OF 20:57 (23 Oct 2026) --------------------------------

    *"The shadow view: switch it back ON in the next build (you fixed the crash cause). I'll test it
     myself with the UnFairchild and the Waves SSL that crashed, since the rig can't load them. If
     either crashes again it goes back off."*

    That is the one test neither the guard nor the rig can run, and he is offering to run it. The fix
    is at the cause and proven by the arithmetic the null came from (SH8/SH8b); what it is not is
    proven against those two plugins, and this switch is how it gets proven. If either crashes again,
    this goes back to false in the same build it is found - and the crash report will say whether it
    is the same defect or a second one, because the two frames that identify it are fixed. */
constexpr bool kShadowViewEnabled = true;


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
        // ---- THE BUFFER IS THE COPY'S, NOT A FIXED STEREO PAIR (his 16:33 crash, 23 Oct 2026) --
        //
        // A WaveShell copy ("SSLComp (s)") crashed the AU hosting process on the render thread:
        //   _platform_memmove  <-  juce::AudioUnitPluginInstanceHeadless::renderGetInputCallback
        //                      <-  AUInputElement::PullInput  (WaveShell1-AU)
        //                      <-  EchoJayProcessor::processBlock
        // SIGSEGV, KERN_INVALID_ADDRESS at 0x0: the AU pulled an input channel this buffer did not
        // have, so JUCE's input callback copied from a null pointer. The buffer was two channels
        // because `prepare` had no instance to ask - and the one thing that knows how many channels
        // a plugin can pull is the plugin.
        //
        // So it is sized HERE, on the message thread, from the copy's own channel counts, and never
        // smaller than stereo. The allocation stays off the audio thread exactly as before: pump()
        // still only reads and writes what this call laid out.
        const int wantCh = juce::jmax (2, inst->getTotalNumInputChannels(),
                                          inst->getTotalNumOutputChannels());
        if (buf_.getNumChannels() != wantCh || buf_.getNumSamples() < kBufFrames)
            buf_.setSize (wantCh, kBufFrames, false, true, false);
        buf_.clear();
        // ---- AND EVERY KEY BUS IS FED, NOT MERELY OWNED (his 12:14 crash, 24 Oct 2026) --------
        //
        // Sizing the buffer to the copy stopped US copying from a null pointer. It did not stop the
        // PLUGIN reading one: a declared input bus handed nothing is exactly what the Tube-Tech
        // read at 0x0, in his Logic and again on the bench. The plan is ChainHost's own self-keying
        // rule (EJShadowChannels.h), computed once here, applied by pump() before every render.
        const auto plan = echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*inst));
        if (echojay::highestPlannedChannel (plan) >= wantCh)
        {
            // A plan this buffer cannot hold is a refusal, never a partial feed: half a key bus is
            // the same null read one channel further along.
            whyNot = "that plugin's key inputs do not fit the copy's buffer";
            return false;
        }
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
            keyPlan_ = plan;     // the copy's shape, fixed for as long as the copy is installed
        }
        uid_ = uid; slot0_ = slot0; name_ = pluginName;
        framesPumped_.store (0, std::memory_order_relaxed);
        blocksSkipped_.store (0, std::memory_order_relaxed);
        on_.store (true, std::memory_order_release);
        return true;
    }

    /** Take it down. The audio thread may be inside pump(), so the flag goes first and the
        instance is let go of under the lock the audio thread only tries - never while it holds it.
        The RING is not unmapped here: its owner (the processor, which mapped the file) does that,
        and only after this has returned. */
    void stop()
    {
        on_.store (false, std::memory_order_release);
        std::unique_ptr<juce::AudioProcessor> leaving;
        {
            const juce::SpinLock::ScopedLockType sl (lock_);
            leaving = std::move (inst_);
            ring_ = nullptr;
            keyPlan_.clear();   // the plan belongs to the copy and dies with it
        }
        // ---- IT IS NEVER DESTROYED HERE (his 14:27 crash, 24 Oct 2026) -------------------------
        //
        // This line was `dying.reset()`. Sean's ruling of 6 Oct says a hosted third-party AU is
        // never disposed mid-session, because a disposed Softube CL 1B left an NSWindow observer
        // registered and a notification 44 s later went into it; the borrow pool has obeyed that
        // since, and his log of 24 Oct shows this road disposing one in the line above the pool
        // parking one. Two minutes later his Logic died inside that plugin's own code, on the main
        // thread, under CoreFoundation's notification callout. The whole account is in
        // EJShadowKeep.h, including the part where the bench could NOT reproduce it.
        juce::String whereItWent;
        const auto was = name_;
        if (echojay::shadowKeepCopy (std::move (leaving), whereItWent))
            EchoJay_NSLog (("EJShadow: the copy of \"" + was + "\" is NOT disposed - "
                            + whereItWent + ". A hosted third-party AU is never disposed "
                            "mid-session (his 14:27 crash, and Sean's ruling of 6 Oct)")
                           .toRawUTF8());
        else
            leaving.reset();        // ours, and ours tear down safely - the only disposal left here
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
        // ---- NOT WHILE THE MESSAGE THREAD IS INSIDE A PLUGIN (his 08:58 crash, 24 Oct 2026) ----
        //
        // The third live-view crash, and the first that was not the channel count: the main thread
        // was eighteen seconds deep in the Tube-Tech's own interface (createEditor -> its Metal
        // pipelines) while this line rendered a copy of that same plugin, and the plugin died at
        // 0x0 in its own render. The two instances were DIFFERENT objects, so lock_ below could
        // never have seen it - what they shared was the plugin's per-process machinery.
        //
        // Skipped rather than queued: there is nothing to catch up on, because a copy whose picture
        // is still being built has no meters to move, and every block here is discarded anyway.
        // COUNTED, so "it froze for a moment" has an answer that is not a guess. The whole rule,
        // and the stack it came from, is in EJPluginCallWindow.h.
        if (insidePluginCall())
        {
            blocksSkipped_.fetch_add (1, std::memory_order_relaxed);
            return 0;
        }
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
                // ...AND EVERY CHANNEL BEYOND THE PAIR IS SILENCE, NOT LEFTOVERS. A surround leg
                // or a metering input the ring knows nothing about is fed zeroes: the tap carries
                // one stereo pair, and handing a plugin last block's samples on a channel we cannot
                // fill would be a reading of something that was never played.
                for (int ch = 2; ch < buf_.getNumChannels(); ++ch)
                    buf_.clear (ch, 0, numSamples);
                // ...EXCEPT A KEY BUS, WHICH IS SELF-KEYED (his 12:14 crash, 24 Oct 2026). Silence
                // on a key is not neutral - it is the 4 Oct defect Kathy measured (a compressor
                // doing nothing, silently) and, on the Tube-Tech, a null read inside the plugin.
                // The same arithmetic ChainHost gives the rack's own slot, so the copy hears what
                // the rack's plugin hears. The plan is fixed while the copy is installed, so this
                // is a copy, never a decision.
                for (const auto& c : keyPlan_)
                    buf_.copyFrom (c.destChannel, 0, buf_, c.srcChannel, 0, numSamples);
                const float inPk = juce::jmax (buf_.getMagnitude (0, 0, numSamples),
                                               buf_.getMagnitude (1, 0, numSamples));
                juce::MidiBuffer noMidi;
                // ALL of the copy's channels, which is what the crash was about: a view narrower
                // than the plugin is a null pointer waiting for the plugin to pull it.
                juce::AudioBuffer<float> view (buf_.getArrayOfWritePointers(),
                                               buf_.getNumChannels(), 0, numSamples);
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

    /** ---- THE COPY'S OWN EDITOR, FOR THE V2'S PANEL (his ruling of 23 Oct) ------------------
        *"when I select a Link's plugin in the V2, I see that plugin's real editor live in the V2
         (moving meters and gain reduction) without opening the Link's window."*

        The guest's own component, created on the message thread, owned by the caller (the panel
        parents it and deletes it when it goes). nullptr when nothing is running or the plugin has
        no editor of its own - and nullptr is a fallback to the static picture, not an error.

        VIEW ONLY, this round: the caller is responsible for dropping the gestures (its own
        interceptor sits over this component), because a guest editor cannot be asked to grey its
        controls. docs/SHADOW_VIEW_STAGES_2026-10-23.md, §3.1. */
    juce::AudioProcessorEditor* createEditorIfPossible()
    {
        if (! running() || inst_ == nullptr) return nullptr;
        if (! inst_->hasEditor()) return nullptr;
        // THE WINDOW IS RAISED FOR THIS CALL, and it has to be raised HERE: the death marks cover
        // every road through ChainHost, and this one does not go through ChainHost - it is the copy
        // asking its own instance. Without it, the one instance that is certainly being rendered
        // would have its interface built underneath the render (his 08:58 crash, 24 Oct 2026).
        const PluginCallWindow noRenderWhileWeAreInHere;
        // ---- AND IT IS MARKED, SO A PLUGIN THAT DIES IN HERE DOES NOT GET A SECOND GO --------
        // Building a guest interface is one of the two calls the live view makes into a third
        // party with exact attribution (the other is the seed), which is why these two are marked
        // and rendering is not - EJShadowState.h says why at length.
        int mark = 0;
        if (auto* hosted = dynamic_cast<juce::AudioPluginInstance*> (inst_.get()))
            mark = echojay::shadowMarkRisky (hosted->getPluginDescription(),
                                             "building its interface for the live view");
        auto* ed = inst_->createEditorIfNeeded();
        echojay::shadowClearRisky (mark);
        if (ed != nullptr)
        {
            // ---- CLICKS DO NOTHING, THIS ROUND (his ruling of 23 Oct 2026) -----------------
            //
            // *"No control through it yet: clicks do nothing."*  A turned knob on the COPY would
            // move the copy and not the Link, so the picture would start disagreeing with the
            // plugin it is a picture of - two sources of truth for one value, which is the family
            // of fault this week was made of. Stage C is what makes a gesture travel.
            //
            // AND THE HONEST LIMIT: this is JUCE's own interception, so it covers a JUCE-drawn
            // editor completely. A guest AU that draws its own NSView may still receive clicks on
            // that view - the plan's note on guest NSViews - so the report says "clicks are not
            // wired to anything" rather than "clicks are impossible", and stage C is where the
            // road is actually built.
            ed->setInterceptsMouseClicks (false, false);
        }
        return ed;
    }

private:
    juce::SpinLock                           lock_;
    std::unique_ptr<juce::AudioProcessor>    inst_;      // under lock_
    void*                                    ring_ = nullptr;   // under lock_; owned by the caller
    juce::AudioBuffer<float>                 buf_;      // allocated at prepare, audio thread's own
    // THE KEY-BUS PLAN, computed on the message thread in start() and read on the audio thread
    // under lock_ with it. Never built in pump(): it is the copy's shape, which cannot change
    // while the copy is installed, and the audio thread allocates nothing.
    std::vector<echojay::ShadowChannelCopy> keyPlan_;   // under lock_
    std::atomic<bool>                        on_ { false };
    std::atomic<float>                       inDb_ { -200.0f }, outDb_ { -200.0f }, grDb_ { 0.0f };
    std::atomic<juce::int64>                 framesPumped_ { 0 };
    std::atomic<int>                         blocksSkipped_ { 0 };
    double                                   hostRate_ = 0.0;
    juce::String                             uid_, name_;
    int                                      slot0_ = -1;
};

}   // namespace echojay
