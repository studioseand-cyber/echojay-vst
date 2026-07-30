/*
    EedDelayProcessor.h  —  "EchoJay Delay": a stereo delay, fully dialable.

    Wave 1, Time cluster (BUILTIN_SUITE_PLAN.md). The DSP lives in DelayEngine
    (JUCE-free, g++-tested); this file is the device: its schema, its id->knob
    map, and its registry entry. Everything that integrates it — the add-menu,
    the [AVAILABLE BUILTINS] advertisement, ChainHost's load dispatch — is
    generated from the registrar at the bottom of the .cpp.

    ELEVEN dialable params, which is the point: time (free or tempo-synced),
    feedback, mix, both loop filters, ping-pong, stereo offset and modulation are
    each a ParamSpec, so each is settable exactly by the model and turnable by
    hand through the SAME path. There is no control on this device that the AI
    cannot reach.

    LATENCY: none. A delay is not lookahead — it emits its dry signal
    immediately and its wet signal late, which is the effect. getLatencySamples()
    stays 0 and the host needs no compensation. getTailLengthSeconds() is
    overridden, though: a delay that reports a zero tail gets its repeats
    truncated by an offline render.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedDelayEngine.h"

class EedDelayProcessor : public EedDeviceProcessor
{
public:
    EedDelayProcessor() = default;

    const juce::String getName() const override { return "EchoJay Delay"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // Repeats keep sounding after the input stops; a host rendering offline has
    // to be told how long, or it cuts the tail off at the end of the region.
    double getTailLengthSeconds() const override;

    // ---- the dialable contract --------------------------------------------
    // Static so the registrar can publish the schema without constructing a
    // device: the advertisement has to exist before anything is instantiated.
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical param ids — used by the editor so a typo cannot silently
    // decouple a knob from the schema entry it is supposed to drive.
    static constexpr const char* kTimeMs       = "time_ms";
    static constexpr const char* kSync         = "sync";
    static constexpr const char* kDivision     = "sync_division";
    static constexpr const char* kFeedback     = "feedback";
    static constexpr const char* kMix          = "mix";
    static constexpr const char* kFilterHpHz   = "filter_hp_hz";
    static constexpr const char* kFilterLpHz   = "filter_lp_hz";
    static constexpr const char* kPingPong     = "ping_pong";
    static constexpr const char* kStereoOffset = "stereo_offset";
    static constexpr const char* kModRateHz    = "mod_rate_hz";
    static constexpr const char* kModDepthMs   = "mod_depth_ms";
    static constexpr const char* kMode         = "mode";
    static constexpr const char* kDiffusion    = "diffusion";
    static constexpr const char* kDuck         = "duck";

    echojay::DelayEngine& engine() noexcept { return engine_; }
    const echojay::DelayEngine& engine() const noexcept { return engine_; }

private:
    echojay::DelayEngine engine_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedDelayProcessor)
};
