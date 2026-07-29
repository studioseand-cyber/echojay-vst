/*
    EedTapeProcessor.h  —  "EchoJay Tape": tape saturation plus the transport.

    A face on the Harmonic cluster's core (BUILTIN_SUITE_PLAN.md §4). The DSP is
    in TapeEngine; this file is the knobs, the contract and the registry entry.

    LATENCY. This device reports it — the transport's fixed 2.5 ms centre delay
    (which is what lets wow modulate in both directions) plus the oversampler's
    45 samples, about 165 samples at 48 kHz. It is published through
    setLatencySamples so the host compensates it.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedTapeEngine.h"
#include "viz/VizTap.h"

class EedTapeProcessor : public EedDeviceProcessor
{
public:
    EedTapeProcessor();

    const juce::String getName() const override { return "EchoJay Tape"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // The transport delay is real time the signal spends inside the device, so
    // it belongs in the tail as well as in the reported latency.
    double getTailLengthSeconds() const override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kSpeedIps  = "speed_ips";
    static constexpr const char* kDriveDb   = "drive_db";
    static constexpr const char* kBias      = "bias";
    static constexpr const char* kWow       = "wow";
    static constexpr const char* kFlutter   = "flutter";
    static constexpr const char* kHeadBump  = "head_bump_db";
    static constexpr const char* kMix       = "mix";
    static constexpr const char* kOutputDb  = "output_db";

    echojay::TapeEngine& engine() noexcept { return engine_; }

    // The editor's HarmonicBars: the device's OUTPUT, left channel. See the
    // note on EedSaturationProcessor::spectrumTap for why one channel and not
    // a mono sum.
    const echojay::viz::SpectrumTap& spectrumTap() const noexcept { return spectrumTap_; }

private:
    echojay::TapeEngine       engine_;
    echojay::viz::SpectrumTap spectrumTap_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedTapeProcessor)
};
