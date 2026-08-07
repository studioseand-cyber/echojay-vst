/*
    EedKeyDetectorProcessor.h  —  "EchoJay Key Detector", the suite's first
    READER (KEY_DETECTOR_SPEC.md). It processes no audio: processBlock feeds
    the engine's lock-free tap and returns the buffer untouched, bit for bit,
    at every setting. Its output is the KeyReading — shown in the editor,
    published to the AI feed as [DETECTED KEY].

    The primary interaction is ANALYSE-on-demand: `analyse` (dialable, so the
    AI can say "listen again") arms a committed pass over the next `window_s`
    of playback; the result holds until re-armed. `continuous` is the optional
    always-updating mode, off by default.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedKeyEngine.h"
#include "EedKeyWorker.h"

class EedKeyDetectorProcessor : public EedDeviceProcessor
{
public:
    EedKeyDetectorProcessor() = default;

    const juce::String getName() const override { return "EchoJay Key Detector"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical ids in one place, so schema, apply path and editor cannot
    // drift apart through a typo.
    static constexpr const char* kAnalyse     = "analyse";
    static constexpr const char* kWindowS     = "window_s";
    static constexpr const char* kContinuous  = "continuous";
    static constexpr const char* kSensitivity = "sensitivity";
    static constexpr const char* kTuningHz    = "tuning_hz";
    static constexpr const char* kAutoTuning  = "auto_tuning";
    static constexpr const char* kModeLock    = "mode_lock";
    static constexpr const char* kHold        = "hold";
    static constexpr const char* kHpss        = "hpss";
    static constexpr const char* kLowHz       = "low_hz";
    static constexpr const char* kHighHz      = "high_hz";
    static constexpr const char* kReset       = "reset";

    echojay::KeyEngine& engine() noexcept             { return engine_; }
    const echojay::KeyEngine& engine() const noexcept { return engine_; }

    // Reading age for the feed's honesty ("age: 4 s"); 0 = never read a key.
    juce::uint32 readingChangeMs() const noexcept { return worker_.lastChangeMs(); }

private:
    // Engine before worker: the worker references the engine and must be
    // destroyed (thread stopped) first.
    echojay::KeyEngine engine_;
    EedKeyWorker       worker_ { engine_ };
    bool               workerStarted_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedKeyDetectorProcessor)
};
