/*
    EedGainProcessor.h  —  "EchoJay Gain": level + pan.

    One of the two devices Wave 0 ships to prove the framework end to end
    (BUILTIN_SUITE_PLAN.md Wave 0 step 4). It is deliberately trivial DSP so that
    everything interesting about it is the PATTERN every later device copies:

        registry entry -> add-menu -> chain host -> editor on the shared look
                       -> dialled by settings_structured.params

    Note what is NOT here: no name matching, no add-menu entry, no advertisement
    text, no dispatch branch. All of that is generated from the registry entry at
    the bottom of the .cpp. This file is the whole device.

    THE ONE ADDITION BEYOND THE DSP: four float taps, feeding the editor's I/O
    meters (VISUALS_PLAN.md, Utility: "Gain: LevelMeter I/O (float tap)"). A gain
    device is the one place a picture of the LEVEL is the whole point — "is this
    move making it louder, and is it about to clip" is the question the device
    exists to answer, and two numbers on a dial cannot answer it.

    The taps are echojay::viz::FloatTap, the lock-free single-float contract in
    viz/VizTap.h. They do not touch the dialable contract: nothing here is a
    ParamSpec, nothing is advertised, and applyStructured is unchanged. Viz is
    read-only (VISUALS_PLAN.md, "Rules carried over").
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedGainEngine.h"
#include "viz/VizTap.h"

class EedGainProcessor : public EedDeviceProcessor
{
public:
    EedGainProcessor() = default;

    const juce::String getName() const override { return "EchoJay Gain"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    // Static so the registrar can publish the schema without constructing a
    // device: the advertisement has to exist before anything is instantiated.
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical param ids — used by the editor so a typo cannot silently
    // decouple a knob from the schema entry it is supposed to drive.
    static constexpr const char* kLevelDb = "level_db";
    static constexpr const char* kPan     = "pan";

    // The depth pass (DEVICE_DEPTH_PLAN.md, Utility): mid/side gain, mono sum,
    // per-channel polarity — the whole utility job in one device.
    static constexpr const char* kMode       = "mode";
    static constexpr const char* kMidDb      = "mid_db";
    static constexpr const char* kSideDb     = "side_db";
    static constexpr const char* kMono       = "mono";
    static constexpr const char* kPhaseLeft  = "phase_left";
    static constexpr const char* kPhaseRight = "phase_right";

    echojay::GainEngine& engine() noexcept { return engine_; }

    // ---- I/O levels for the editor's meters --------------------------------
    // Linear magnitude, not dB: the meter converts, and a linear zero is an
    // honest silence where a dB zero is full scale. Read on the editor's timer.
    //
    // Measured across whatever channels the host gave us — peak is the loudest
    // single sample on ANY channel (that is what clips), RMS is pooled across
    // them (that is what it sounds like).
    float inputPeak()   const noexcept { return inPeak_.get(); }
    float inputRms()    const noexcept { return inRms_.get(); }
    float outputPeak()  const noexcept { return outPeak_.get(); }
    float outputRms()   const noexcept { return outRms_.get(); }

private:
    // Peak + pooled RMS of `numCh` channels, published to a pair of taps.
    static void publishLevels (const juce::AudioBuffer<float>& buffer, int numCh,
                               echojay::viz::FloatTap& peakTap,
                               echojay::viz::FloatTap& rmsTap);

    echojay::GainEngine engine_;

    echojay::viz::FloatTap inPeak_, inRms_, outPeak_, outRms_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedGainProcessor)
};
