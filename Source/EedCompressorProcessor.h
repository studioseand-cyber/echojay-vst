/*
    EedCompressorProcessor.h  —  "EchoJay Compressor".

    The first face on the shared dynamics core (BUILTIN_SUITE_PLAN.md §4). There
    is deliberately no EedCompressorEngine: DynamicsCore IS the engine, JUCE-free
    and g++-tested in test/dynamics_core_test.cpp, and wrapping it in a per-device
    engine that only forwards setters would add a layer that can drift from the
    schema without adding anything the DSP needs. Same documented exception as
    EedPhaseInvertProcessor, for the same reason: a layer that cannot be wrong is
    better than a layer that merely happens to be right.

    DETECTOR. RMS with a 10 ms window by default — peak detection makes a
    compressor chase individual transients, which is what a LIMITER is for, while
    RMS tracks loudness, which is what makes a compressor sound like it is riding
    a fader. It was originally fixed for exactly that reason; the depth pass
    publishes it, because "peak" is the right answer often enough (a bass DI, a
    kick) that committing on the user's behalf was the wrong call, and a default
    is a much cheaper way to express an opinion than a missing control.

    THE DEPTH PASS (DEVICE_DEPTH_PLAN.md, Dynamics). Everything this device gained
    lives in the shared core, so all six faces gained it too:

      * `mode` — the character: clean (VCA), glue (bus), punch (FET), smooth
        (opto). It reshapes attack, release and knee and adds gentle drive as the
        reduction deepens. THE marquee control: one param that changes how the
        device feels, rather than six the model has to derive.
      * `sc_hpf_hz` — high-pass the DETECTOR so bass does not pump the whole mix.
      * `lookahead_ms` — reported to the host as latency, like the limiter's.
      * `auto_release` — programme-dependent recovery.
      * `stereo_link` — 100% is one gain for both channels; below that the
        channels are measured separately.
      * `range_db` — a ceiling on the gain reduction, so "squash it but never by
        more than 6 dB" is one number rather than a compromise on the ratio.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedDynamicsCore.h"

class EedCompressorProcessor : public EedDeviceProcessor
{
public:
    EedCompressorProcessor();

    const juce::String getName() const override { return "EchoJay Compressor"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;

    // ---- the dialable contract --------------------------------------------
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    // Canonical param ids — used by the editor so a typo cannot silently
    // decouple a knob from the schema entry it drives.
    static constexpr const char* kThresholdDb = "threshold_db";
    static constexpr const char* kRatio       = "ratio";
    static constexpr const char* kAttackMs    = "attack_ms";
    static constexpr const char* kReleaseMs   = "release_ms";
    static constexpr const char* kKneeDb      = "knee_db";
    static constexpr const char* kMakeupDb    = "makeup_db";
    static constexpr const char* kMix         = "mix";

    static constexpr const char* kMode        = "mode";
    static constexpr const char* kScHpfHz     = "sc_hpf_hz";
    static constexpr const char* kLookaheadMs = "lookahead_ms";
    static constexpr const char* kAutoRelease = "auto_release";
    static constexpr const char* kDetector    = "detector";
    static constexpr const char* kStereoLink  = "stereo_link";
    static constexpr const char* kRangeDb     = "range_db";

    // The ceiling the lookahead buffer is sized for, once, in prepareToPlay, and
    // the schema's maximum: asking for more than the buffer holds would be an
    // allocation on the audio thread, which the real-time contract forbids.
    static constexpr double kMaxLookaheadMs = 10.0;

    // What the character mode made of the dialled attack / release / knee. The
    // editor draws and reports from these, not from the dialled values, or the
    // picture would show a 6 dB knee on a device running 15.
    double effectiveAttackMs()  const noexcept { return core_.effectiveAttackMs(); }
    double effectiveReleaseMs() const noexcept { return core_.effectiveReleaseMs(); }
    float  effectiveKneeDb()    const noexcept { return core_.effectiveKneeDb(); }
    echojay::CharacterMode character() const noexcept { return core_.getCharacter(); }

    // For the editor's GR meter. Negative dB, 0 when not reducing.
    float gainReductionDb() const noexcept { return core_.gainReductionDb(); }

    // The level the detector is seeing this instant, dBFS. Same one-float
    // contract as the line above. Only a whisper of extra brightness on the
    // curve's glow now — the shape itself comes from the histogram below.
    float detectorLevelDb() const noexcept { return core_.detectorLevelDb(); }

    // Where the signal LIVES on that curve — the dwell histogram behind the
    // transfer curve's glow. Same never-block contract as the floats above,
    // published whole so the shape is never half of two different moments.
    const echojay::dyn::DwellTap& dwellHistogram() const noexcept
    {
        return core_.dwellHistogram();
    }

    // The transfer curve the editor draws is drawn from the core's OWN gain
    // computer, not from a second copy of "threshold, ratio, knee" in UI code.
    // Read-only use: the editor dials through setParamValue like everything
    // else, so there is no second write path into the DSP.
    const echojay::DynamicsCore& core() const noexcept { return core_; }

private:
    echojay::DynamicsCore core_;

    // The REQUESTED lookahead, kept here rather than read back off the delay
    // line: the ring rounds a request to whole samples, so asking the core would
    // make a state round-trip give back 1.9998 ms for the 2 the user set, and a
    // session would drift a little further from what was dialled every time it
    // was saved.
    double lookaheadMs_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedCompressorProcessor)
};
