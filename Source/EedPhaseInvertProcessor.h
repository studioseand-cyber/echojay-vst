/*
    EedPhaseInvertProcessor.h  —  "EchoJay Phase Invert": per-channel polarity.

    The second Wave 0 proof device. There is deliberately no EedPhaseInvertEngine:
    the DSP is a sign flip, and wrapping that in a core with its own g++ test
    would be ceremony rather than coverage. The device template says to add an
    engine when there is DSP worth testing in isolation — this is the documented
    exception, not an oversight.

    Two independent switches rather than one. A single "polarity" control handles
    the common case (a mic flipped against another), but the reason you reach for
    this device at all is often a channel-specific problem — a mis-wired cable on
    one side of a stereo pair — and a one-knob version cannot fix that.

    VISUALISATION: one float, and deliberately nothing more. VISUALS_PLAN.md says
    this device "stays minimal (a small correlation dot at most)", and that is the
    right call — there is no curve, no spectrum and no envelope here, and a device
    with nothing to show should not be given a panel to show it in.

    Correlation is the exception worth publishing, because it is the one number
    this device exists to move: flipping one side of a correlated pair drives it
    to -1, which is precisely the mono fold-down cancellation that makes polarity
    worth fixing. It is measured on the OUTPUT, after the flip, so it reports the
    state the device is actually leaving the signal in.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "viz/VizTap.h"

class EedPhaseInvertProcessor : public EedDeviceProcessor
{
public:
    EedPhaseInvertProcessor() = default;

    const juce::String getName() const override { return "EchoJay Phase Invert"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kInvertL = "invert_left";
    static constexpr const char* kInvertR = "invert_right";

    // ---- correlation, for the editor's dot ---------------------------------
    // -1 fully out of phase (a mono fold-down cancels), 0 uncorrelated,
    // +1 fully in phase. kNoCorrelation when there is no stereo pair to
    // correlate at all — a sentinel OUTSIDE the valid range rather than a 0,
    // because "mono, nothing to say" and "stereo, fully decorrelated" are
    // different facts and a meter that draws them identically is lying about one.
    static constexpr float kNoCorrelation = 2.0f;

    float correlation() const noexcept { return corrTap_.get(); }

private:
    void publishCorrelation (const juce::AudioBuffer<float>& buffer, int numCh, int numSamples);

    // Atomics, not a lock: the audio thread reads these every block while the
    // message thread (or an AI move) writes them.
    std::atomic<bool> invertL_ { false };
    std::atomic<bool> invertR_ { false };

    // Flipping polarity is a jump from +1 to -1 — the largest discontinuity a
    // sample can have, and an audible click on anything but silence. The
    // multiplier ramps instead, so a flip is a ~5 ms dip through zero. Same
    // no-click discipline as GainEngine; an AI move can land mid-playback.
    float coeffL_    = 1.0f;      // audio thread only
    float coeffR_    = 1.0f;
    float rampCoeff_ = 0.0f;

    echojay::viz::FloatTap corrTap_ { kNoCorrelation };

    // Smoothed across blocks, audio thread only. A block-by-block correlation
    // on real material jitters far too fast to read; this is a display number,
    // and the eye wants the trend rather than the instantaneous value.
    float corrSmoothed_ = 0.0f;
    bool  corrPrimed_   = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPhaseInvertProcessor)
};
