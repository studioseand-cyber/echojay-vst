/*
    EedLimiterProcessor.h  —  "EchoJay Limiter".

    DynamicsCore in Limit mode — an infinite ratio, so whatever goes in above the
    ceiling comes out AT the ceiling — plus the one thing the other five faces do
    not have: LOOKAHEAD.

    WHY LOOKAHEAD, AND WHAT IT COSTS. A limiter with no lookahead has to choose
    between an attack fast enough to catch a transient (which distorts, because
    a per-sample gain change on a low-frequency waveform IS distortion) and an
    attack slow enough to be clean (which lets the transient through). Lookahead
    removes the choice: the DETECTOR reads the input undelayed while the SIGNAL
    is delayed, so the gain is already where it needs to be by the time the peak
    arrives, and the attack can be gentle.

    The cost is latency, and it is REPORTED, via setLatencySamples(). This is not
    optional politeness: an unreported delay puts this track out of time with
    every other track in the session, and the error is a few milliseconds — small
    enough to sound like a mix problem rather than like a bug. ChainHost sums the
    reported latency of every slot (getTotalLatencySamples) and PluginProcessor
    mirrors it to the DAW, so a correct number here is all the device owes.

    ATTACK IS DERIVED, not published. It is tied to the lookahead — roughly a
    third of it — because those are the two halves of one decision: an attack
    slower than the lookahead lets peaks past, and one much faster throws away
    the transparency the lookahead was bought for. Publishing both would let the
    model set a combination that is simply wrong, and it would have no way to
    know that from the schema.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedDynamicsCore.h"

class EedLimiterProcessor : public EedDeviceProcessor
{
public:
    EedLimiterProcessor();

    const juce::String getName() const override { return "EchoJay Limiter"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kCeilingDb   = "ceiling_db";
    static constexpr const char* kReleaseMs   = "release_ms";
    static constexpr const char* kLookaheadMs = "lookahead_ms";

    // The ceiling the lookahead buffer is sized for, once, in prepareToPlay.
    // Also the schema's maximum: asking for more than the buffer holds would be
    // an allocation on the audio thread, which the real-time contract forbids.
    static constexpr double kMaxLookaheadMs = 10.0;

    float gainReductionDb() const noexcept { return core_.gainReductionDb(); }
    float detectorLevelDb()  const noexcept { return core_.detectorLevelDb(); }

    // Where the signal LIVES on that curve — the dwell histogram behind the
    // transfer curve's glow. Same never-block contract as the floats above,
    // published whole so the shape is never half of two different moments.
    const echojay::dyn::DwellTap& dwellHistogram() const noexcept
    {
        return core_.dwellHistogram();
    }

private:
    // Recompute the delay, the derived attack and the reported latency together.
    // They are three views of one decision, so they are never updated apart.
    void applyLookahead();

    echojay::DynamicsCore   core_;
    echojay::LookaheadDelay delay_;

    double lookaheadMs_ = 2.0;
    double sampleRate_  = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterProcessor)
};
