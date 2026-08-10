/*
    EedPitchProcessor.h  —  "EchoJay Pitch", device #22
    (PITCH_CORRECTION_SPEC.md), at build phase P0: DETECTION ONLY.

    In this phase the device is a READER: processBlock feeds the YIN engine and
    returns the buffer untouched, bit for bit, at every setting. No shifting,
    no scale logic, no correction — those are P1+ and gate on this detector
    being solid, because a correction artefact caused by a detection error
    looks exactly like a PSOLA bug and gets debugged as one.

    What it shows (the P0 debug readout): detected pitch as note + cents, f0,
    confidence, the voiced/unvoiced flag, and how often the octave-error guard
    has fired — the number the spec says to log.
*/

#pragma once

#include "EedDeviceProcessor.h"
#include "EedPitchEngine.h"
#include "EedPsolaEngine.h"
#include "EedPitchCorrect.h"
#include "EedKeyFeed.h"

class EedPitchProcessor : public EedDeviceProcessor
{
public:
    EedPitchProcessor() = default;

    const juce::String getName() const override { return "EchoJay Pitch"; }

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
    static constexpr const char* kVoiceType  = "voice_type";
    static constexpr const char* kTracking   = "tracking";
    static constexpr const char* kTargetHz   = "target_hz";
    static constexpr const char* kFormantMode = "formant_mode";
    static constexpr const char* kLowLatency  = "low_latency";
    // P2: the musical layer.
    static constexpr const char* kCorrect      = "correct";
    static constexpr const char* kRetuneMs     = "retune_speed_ms";
    static constexpr const char* kFlex         = "flex";
    static constexpr const char* kHumanize     = "humanize";
    static constexpr const char* kKeyRoot      = "key_root";
    static constexpr const char* kScale        = "scale";
    static constexpr const char* kReferenceHz  = "reference_hz";
    static constexpr const char* kTranspose    = "transpose";
    static constexpr const char* kIgnoreVib    = "targeting_ignores_vibrato";
    static constexpr const char* kMode         = "correction_mode";
    static constexpr const char* kMix          = "mix";
    static constexpr const char* kOutputDb     = "output_db";
    static constexpr const char* kKeySource    = "key_source";
    static constexpr const char* kRefSource    = "reference_source";

    // What the auto-map resolved to, for the editor's attribution line. A
    // wrong reading is only diagnosable when the user can see where it came
    // from, so the source name travels with the value.
    struct AutoKeyState
    {
        bool         active   = false;   // key_source is auto
        bool         applied  = false;   // a key was actually taken
        bool         fellBack = false;   // gated -> chromatic
        int          root     = 0;
        bool         minor    = false;
        float        conf     = 0.0f;
        float        tuningHz = 440.0f;
        juce::String sourceName;
    };
    AutoKeyState autoKeyState() const;

    // correction_mode indices. `custom` is LAST and is what the display falls
    // to the moment any of the params a mode writes is moved by hand.
    enum Mode { kNatural = 0, kBalanced, kTuned, kHard, kCustom, kNumModes };

    // Index of `custom` in the scale choices - a DISPLAY state reached by
    // editing a degree, never a mask to write.
    static constexpr int kScaleCustom = 10;
    // Indices into the scale choices, used by the auto-map.
    static constexpr int kScaleMajor = 0, kScaleMinor = 1, kScaleChromatic = 9;

    // The pitch_scale array move (spec §5.1). MERGE semantics keyed on
    // `semitone`: an omitted degree keeps its current state.
    juce::String applyStructured (const juce::var& structured,
                                  int* appliedOut = nullptr,
                                  int* skippedOut = nullptr) override;

    // The two lookahead settings, in periods of the active voice_type's floor.
    // PUBLIC because the editor prints the latency each one causes - the number
    // is the whole point of the control, so the control has to be able to ask.
    //
    // Both are measured rather than chosen (PITCH_P0_VALIDATION.md §8): at 3.0
    // periods onset crest matches the dry signal exactly (+0.00 dB), at 2.0 it
    // is +0.63 dB, and by 1.75 the tracking-lag compensation starts being
    // truncated and crest diverges past +1.3 dB.
    static constexpr float kLookaheadMixing   = 3.0f;
    static constexpr float kLookaheadTracking = 2.0f;
    static constexpr const char* kResetStats = "reset_stats";

    echojay::PitchEngine&       engine() noexcept       { return engine_; }
    const echojay::PitchEngine& engine() const noexcept { return engine_; }

    echojay::PitchCorrect&       corrector() noexcept       { return correct_; }
    const echojay::PitchCorrect& corrector() const noexcept { return correct_; }

    echojay::PsolaEngine&       shifter() noexcept       { return psola_; }
    const echojay::PsolaEngine& shifter() const noexcept { return psola_; }

private:
    // Latency depends on voice_type (the lowest pitch the window must
    // represent), so it is recomputed whenever that changes and the host is
    // told. Reported honestly per spec §2.4 - a corrector that silently
    // misaligns a vocal against the track is worse than a slower one.
    void refreshLatency();

    echojay::PitchEngine engine_;
    echojay::PsolaEngine  psola_;
    echojay::PitchCorrect correct_;
    // These three MUST match their schema defaults. They did not: correct
    // advertised on but constructed off, correction_mode advertised natural
    // but displayed custom, and scale advertised chromatic but constructed
    // MAJOR - so a freshly added device came up forcing C major, which is
    // exactly the guess the advertisement forbids.
    std::atomic<bool>     correctOn_ { true };
    std::atomic<int>      scaleIndex_ { 9 };   // chromatic

    // Writing a named scale rewrites all twelve degrees. Touching one degree
    // afterwards would move `scale` to custom - the same visible-state rule the
    // spec sets for correction_mode.
    void applyScale (int index);

    // Reads the shared feed and moves key_root/scale/reference_hz when the
    // relevant source is auto. Called per block; only acts on an actual
    // change, so a steady key costs one atomic read.
    void refreshAutoKey();

    std::atomic<bool> keyAuto_ { true };
    std::atomic<bool> refAuto_ { true };
    int   lastAutoRoot_  = -1;
    bool  lastAutoMinor_ = false;
    bool  lastAutoFellBack_ = false;
    float lastAutoTuning_ = 0.0f;
    mutable juce::CriticalSection autoLock_;
    AutoKeyState autoState_;

    // Writes the six visible params a mode stands for, and reports what it
    // moved - a mode that changes behaviour without moving knobs is undialable
    // by definition, and one that moves six knobs under a single line saying
    // "mode = natural" is the same failure wearing a different hat.
    juce::String applyMode (int mode);

    juce::String applyPitchScale (const juce::var& arr, int* applied, int* skipped);

    // Set while a mode is writing its params, so the writes do not each knock
    // the display to custom.
    // Knocks the mode display to custom, unless a mode is doing the writing.
    void toCustomMode() { if (! applyingMode_) modeIndex_.store (kCustom); }

    // applyMode/applyScale run inside setParamValue, whose caller builds its
    // own summary from id/value pairs and would report "correction_mode hard"
    // over six knobs that silently moved. They leave their detail here so
    // applyStructured can put it back into the applied line.
    juce::String pendingModeSummary_, pendingScaleSummary_;

    bool applyingMode_ = false;
    std::atomic<int> modeIndex_ { kNatural };
    double               sampleRate_ = 48000.0;
    int                  latencyVoiceType_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPitchProcessor)
};
