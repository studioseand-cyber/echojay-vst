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

    THE DEPTH PASS added three things (DEVICE_DEPTH_PLAN.md, Dynamics):

      * `mode` — transparent | punchy | clip.
          transparent  the lookahead limiter above, uncoloured.
          punchy       the same, on the core's `punch` character: a faster attack
                       and recovery and a touch of drive as it works, which is
                       what makes a loud master feel dense rather than merely
                       loud.
          clip         a HARD CEILING. Attack and release both go to zero, so the
                       gain is the instantaneous ceiling/peak ratio, which IS
                       clipping — and the lookahead is forced off, because a hard
                       clip has nothing to look ahead FOR. Cheap, obvious, and
                       the loudest of the three; the release dial and the
                       lookahead dial are both meaningless here, and the editor
                       hides them rather than leaving them live and ignored.
      * `true_peak` — detect between the samples, so the ceiling holds against
        what a converter or a codec actually reconstructs rather than against the
        samples alone.
      * `sc_hpf_hz` — a detector high-pass, WITH A CAVEAT the schema carries:
        anything the detector cannot hear can exceed the ceiling, so on a master
        this is a deliberate trade and not a default.
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
    static constexpr const char* kMode        = "mode";
    static constexpr const char* kTruePeak    = "true_peak";
    static constexpr const char* kScHpfHz     = "sc_hpf_hz";

    // The ceiling the lookahead buffer is sized for, once, in prepareToPlay.
    // Also the schema's maximum: asking for more than the buffer holds would be
    // an allocation on the audio thread, which the real-time contract forbids.
    static constexpr double kMaxLookaheadMs = 10.0;

    // The three limiter modes, in the schema's order. Named rather than bare
    // indices because the processor branches on them and "mode_ == 2" in a
    // processBlock is how a reordered schema becomes a silent behaviour change.
    enum class Mode { Transparent = 0, Punchy = 1, Clip = 2 };
    static constexpr int kNumModes = 3;

    Mode mode() const noexcept { return mode_; }

    // Whether the dialled release and lookahead are doing anything at all. The
    // editor asks rather than testing the mode itself, so the interlock is stated
    // once, next to the code that implements it.
    bool releaseInUse()   const noexcept { return mode_ != Mode::Clip; }
    bool lookaheadInUse() const noexcept { return mode_ != Mode::Clip; }

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
    // Recompute the delay, the derived attack, the release and the reported
    // latency together. They are four views of one decision — the mode and the
    // lookahead between them settle all of it — so they are never updated apart.
    void applyLookahead();

    echojay::DynamicsCore   core_;
    echojay::LookaheadDelay delay_;

    Mode   mode_        = Mode::Transparent;
    double lookaheadMs_ = 2.0;
    double sampleRate_  = 44100.0;

    // The DIALLED release. `clip` drives the core's release to zero, so the core
    // can no longer be asked what the user set — and a state round-trip that gave
    // back 0 ms would quietly rewrite the dial the next time the mode changed.
    double releaseMs_ = 50.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterProcessor)
};
