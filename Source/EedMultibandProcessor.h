/*
    EedMultibandProcessor.h  —  "EchoJay 4-Band Compressor".

    A Linkwitz-Riley tree into four bands, each one its own DynamicsCore, summed
    back. The crossover work is in EedDynamicsCore.h (FourBandSplitter) and is
    measured flat by test/dynamics_core_test.cpp; what is here is the device: four
    compressors, three crossovers, and the dialable contract over all of it.

    TWO SHAPES REACH THE SAME KNOBS. This is the "structured device" of
    BUILTIN_SUITE_PLAN.md §3, the second one after the EQ:

      settings_structured: { "comp_bands": [ {"band": 4, "threshold_db": -24} ] }
      settings_structured: { "params": { "band4_threshold_db": -24 } }

    Both land on the same param. The array form is what a model reaches for when
    it is thinking in bands ("tame the top end"); the flat form is what the rest
    of the suite already speaks. They are not two code paths: applyCompBands
    rewrites its entries into flat ids and hands them to the SAME applyParams the
    base class uses, so clamping, merge semantics and the report are written once.
    A second implementation of clamping is a second set of bugs.

    MERGE SEMANTICS, and the one place they deliberately differ from the EQ. In
    both, a key that is absent is left alone. But an eq_bands entry REPLACES the
    band it targets (an EQ band is a free-floating object you place, so a partial
    set would leave half of a previous band's shape behind), whereas a comp_bands
    entry MERGES into it (a compressor band is a fixed slot defined by the
    crossovers, so "make band 4 faster" must not reset its threshold). Same
    intent — do what was asked and nothing else — expressed differently because
    the two objects are different.

    Every band's every knob is a schema entry (band1_threshold_db ...), so the
    house rule holds: nothing is clickable-but-not-dialable, and the array form
    is a convenience over the contract rather than a hole in it.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedDynamicsCore.h"

class EedMultibandProcessor : public EedDeviceProcessor
{
public:
    EedMultibandProcessor();

    const juce::String getName() const override { return "EchoJay 4-Band Compressor"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    static constexpr int kNumBands = echojay::FourBandSplitter::kNumBands;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Adds the comp_bands array form on top of the base's flat `params`.
    juce::String applyStructured (const juce::var& structured,
                                  int* appliedOut = nullptr,
                                  int* skippedOut = nullptr) override;

    // The array form on its own, for a caller that already has just the array.
    juce::String applyCompBands (const juce::var& compBandsArray,
                                 int* appliedOut = nullptr,
                                 int* skippedOut = nullptr);

    // Flat id for one band's param: bandIndex is 0-based, `leaf` is "threshold_db".
    // Built in ONE place so the schema, the editor and the array path cannot
    // spell the same knob three different ways.
    static juce::String bandParamId (int bandIndex, const char* leaf);

    // The per-band leaves, in the order the editor lays them out.
    static constexpr const char* kThresholdDb = "threshold_db";
    static constexpr const char* kRatio       = "ratio";
    static constexpr const char* kAttackMs    = "attack_ms";
    static constexpr const char* kReleaseMs   = "release_ms";
    static constexpr const char* kKneeDb      = "knee_db";
    static constexpr const char* kMakeupDb    = "makeup_db";
    static constexpr const char* kBypass      = "bypass";

    static constexpr const char* kCrossover1Hz = "crossover1_hz";
    static constexpr const char* kCrossover2Hz = "crossover2_hz";
    static constexpr const char* kCrossover3Hz = "crossover3_hz";

    float gainReductionDb (int band) const noexcept
    {
        return (band >= 0 && band < kNumBands) ? cores_[band].gainReductionDb() : 0.0f;
    }

    bool isBandBypassed (int band) const noexcept
    {
        return (band >= 0 && band < kNumBands) && bandBypass_[band];
    }

    // The crossovers as the splitter actually realised them — it sorts and
    // spaces what it is given, so this is not always what was asked for.
    double crossoverHz (int i) const noexcept;

private:
    void pushCrossovers();

    echojay::FourBandSplitter splitter_;
    echojay::DynamicsCore     cores_[kNumBands];

    bool   bandBypass_[kNumBands] { false, false, false, false };
    double crossoverHz_[3] { 120.0, 800.0, 5000.0 };
    double sampleRate_ = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedMultibandProcessor)
};
