/*
    EedLevelProcessor.h  -  "EchoJay Level" (18e, 19 Sep 2026): the slot the loudness loop drives.

    A clean gain stage (-24..+24 dB, smoothed) placed by the server (or by the client when absent) immediately
    BEFORE whatever limiter sits last - any brand. It meters its own INPUT and its OUTPUT (K-weighted short-term
    LUFS-S and true peak, the same LevelTally the chain uses) and shows both on its card. The loop sets gain_db
    here and never a limiter parameter; the limiter only holds the ceiling. target_lufs / loudness_option ride as
    params so the loop arms from this slot's structured settings (built-ins skip unknown params, so an older build
    that does not know them is unharmed).
*/
#pragma once
#include "EedDeviceProcessor.h"
#include "EchoJayLevelTally.h"
#include <atomic>
#include <limits>

class EedLevelProcessor : public EedDeviceProcessor
{
public:
    EedLevelProcessor() = default;
    const juce::String getName() const override { return "EchoJay Level"; }
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    static const echojay::ParamSchema& schema();
    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kGainDb        = "gain_db";
    static constexpr const char* kTargetLufs    = "target_lufs";
    // 0 commercial, 1 pushed, 2 dynamic, 3 keep, 4 MATCH.
    // 08c item F2 (9 Oct 2026): "match" is the CHANNEL/BUS aim - the chain must not change the level, so the
    // target is the chain's own INPUT, measured, rather than a figure the server chose. It is a loudness option
    // and not a separate field because that is the contract agreed with B for level_params ("match" | "pushed" |
    // "dynamic"), and because every mechanism in the loop then works unchanged: match is "hit the target" with
    // the target measured. A build that carries option 4 and NO target_lufs is valid, and is the only option
    // value for which that is true.
    static constexpr const char* kLoudnessOption = "loudness_option";
    static constexpr double kMinDb = -24.0, kMaxDb = 24.0;

    double gainDb() const noexcept { return gainDb_.load (std::memory_order_relaxed); }
    double targetLufs() const noexcept { return targetLufs_.load (std::memory_order_relaxed); }
    int    loudnessOption() const noexcept { return option_.load (std::memory_order_relaxed); }
    static const char* optionName (int i) noexcept { static const char* n[] = { "commercial", "pushed", "dynamic", "keep", "match" }; return n[juce::jlimit (0, 4, i)]; }
    static constexpr int kOptionMatch = 4;

    // The meters: input (before this gain) and output (after it). Read on the message thread.
    echojay::LevelTally::Snapshot inputLevels()  const { return in_.snapshot(); }
    echojay::LevelTally::Snapshot outputLevels() const { return out_.snapshot(); }
    void resetMeters() { in_.reset(); out_.reset(); in_.resetShortTermMax(); out_.resetShortTermMax(); }
    // 18f: the limiter after this slot - its current gain reduction, written by the loop's tick (NaN = unknown / not an EchoJay limiter)
    void  setDownstreamGrDb (float db, bool estimated = false) noexcept { downstreamGr_.store (db, std::memory_order_relaxed); downstreamGrEst_.store (estimated, std::memory_order_relaxed); }
    bool  downstreamGrEstimated() const noexcept { return downstreamGrEst_.load (std::memory_order_relaxed); }   // 18g: a third-party limiter's GR is an estimate (Level OUT - chain OUT)
    float downstreamGrDb() const noexcept { return downstreamGr_.load (std::memory_order_relaxed); }

private:
    std::atomic<double> gainDb_ { 0.0 }, targetLufs_ { -9.0 };
    std::atomic<int>    option_ { 0 };
    std::atomic<float>  downstreamGr_ { std::numeric_limits<float>::quiet_NaN() };
    std::atomic<bool>  downstreamGrEst_ { false };
    float  curLin_ = 1.0f;          // audio-thread smoothed linear gain
    float  ramp_   = 0.0f;          // per-sample ramp coefficient
    echojay::LevelTally in_ { echojay::LevelTally::Weighting::K }, out_ { echojay::LevelTally::Weighting::K };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLevelProcessor)
};
