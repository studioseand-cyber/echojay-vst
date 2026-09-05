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
#include "viz/PitchRibbonView.h"

class EedPitchProcessor : public EedDeviceProcessor,
                          public echojay::KeyFeedConsumer
{
public:
    EedPitchProcessor();   // consults the schema for every default (one source of truth)

    // KeyFeedConsumer: which instance hosts this device. Used by the auto
    // reference to recognise - and refuse - a tuning grid derived from the
    // very channel being corrected (see refreshAutoKey).
    void setKeyFeedSelfId (uint64_t id) override
    { keyFeedSelfId_.store (id, std::memory_order_relaxed); }

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
    static constexpr const char* kFormantShift = "formant_shift";
    static constexpr const char* kLowLatency  = "low_latency";
    // P2: the musical layer.
    static constexpr const char* kCorrect      = "correct";
    static constexpr const char* kRetuneMs     = "retune_speed_ms";
    static constexpr const char* kFlex         = "flex";
    static constexpr const char* kHumanize     = "humanize";
    static constexpr const char* kDepth        = "depth";
    static constexpr const char* kKeyRoot      = "key_root";
    static constexpr const char* kScale        = "scale";
    static constexpr const char* kReferenceHz  = "reference_hz";
    static constexpr const char* kTranspose    = "transpose";
    static constexpr const char* kIgnoreVib    = "targeting_ignores_vibrato";
    static constexpr const char* kSeamAttackMs = "seam_attack_ms";
    // Provenance marker (29 Aug 2026): 1 once a PERSON has taken manual
    // control of the reference. Saved states lacking it revert a manual
    // reference to auto on load - see onStateApplied.
    static constexpr const char* kRefManualByUser = "ref_manual_by_user";
    static constexpr const char* kMode         = "correction_mode";
    static constexpr const char* kMix          = "mix";
    static constexpr const char* kOutputDb     = "output_db";
    static constexpr const char* kKeySource    = "key_source";
    static constexpr const char* kRefSource    = "reference_source";
    // P5: vibrato.
    static constexpr const char* kNaturalVib   = "natural_vibrato";
    static constexpr const char* kVibDepth     = "vib_depth_cents";
    static constexpr const char* kVibRate      = "vib_rate_hz";
    static constexpr const char* kVibShape     = "vib_shape";
    static constexpr const char* kVibOnset     = "vib_onset_ms";

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
        // The reference line (29 Aug 2026): what grid the corrector is on
        // and WHY - the state whose invisibility cost days.
        bool  refAuto        = false;
        float refApplied     = 440.0f;
        bool  refSelfIgnored = false;   // auto saw only this channel: using 440
        // KEY-SIDE CIRCULARITY (3 Sep 2026, DEFECT_AUTOKEY_PROVENANCE.md §2):
        // a usable fact was ignored for KEY ROOT/MODE because it was derived
        // from this instance's own channel - chromatic applied, shown as such.
        bool  keySelfIgnored = false;
    };
    AutoKeyState autoKeyState() const;

    // THE KEY-SIDE CIRCULARITY GUARD, BEHIND A FLAG (3 Sep 2026, round-21
    // ruling: build behind the flag, measure, report; default flips only by
    // ruling). The reference guard (29 Aug) covers tuning only; key root and
    // mode from the same self-derived fact were applied unguarded - a vocal
    // channel declared a music bus keys the corrector off the singer's own
    // melody. With the flag on, such a fact is UNMEASURED for key too: fall
    // back to chromatic, never to the last key (the asymmetry: a wrong key
    // moves notes a semitone, chromatic declines to snap). DEFAULT ON since
    // 5 Sep 2026 (round-23 ruling); the setter remains for the A/B harness.
    void debugKeySelfGuard (bool on) noexcept { keySelfGuard_.store (on); }
    bool keySelfGuardOn() const noexcept   { return keySelfGuard_.load(); }

    // The retune floor's effective value, for the knob readout (30 Aug
    // 2026: a mapping that lives only in schema text is a mapping the
    // next person doesn't know about).
    float retuneEffectiveMs() const noexcept { return correct_.retuneEffectiveMs(); }
    // Load-clamp memory (2 Sep 2026 cap): when a saved session carries a
    // retune above the new 150ms cap, the value is clamped ON LOAD and the
    // readout says so - "150 (was 400)". Never a silent change to a saved
    // sound. Cleared by any live write.
    float retuneWasMs() const noexcept { return retuneWasMs_; }

protected:
    void onStateApplied() override;
public:

    // Writing the schema defaults must not be mistaken for the user reaching
    // for key_root or scale, which would leave key_source on manual and
    // silently contradict its own advertised default of auto.
    void resetParamsToDefaults() override;

    // The ribbon's frame store. Written on the audio thread once per block and
    // read by paint; a torn column is one pixel and not worth a lock.
    echojay::viz::PitchRibbonView& ribbon() noexcept { return ribbon_; }

    // ---- Retune trace (3 Sep 2026 investigation) --------------------------
    // Per-HOP records of the whole decision: what the corrector saw (in,
    // slow, osc), what it aimed at, what the envelope emitted, and the f0
    // the shifter divides by. Lock-free SPSC ring written at each hop on
    // the audio thread; the pitch editor's timer drains to CSV while open.
    // The 30Hz ribbon cannot answer phase questions at 6Hz; this can.
    struct TraceRec { double tSec; float f0Hz, inC, slowC, oscC, aimC, envC; };
    static constexpr int kTraceCap = 8192;
    std::array<TraceRec, kTraceCap> trace_ {};
    std::atomic<uint32_t> traceW_ { 0 };
    uint32_t traceR_ = 0;   // drained by the editor timer (message thread)
    int drainTrace (TraceRec* out, int maxN) noexcept
    {
        int n = 0;
        const uint32_t w = traceW_.load (std::memory_order_acquire);
        while (traceR_ != w && n < maxN)
        { out[n++] = trace_[traceR_ % (uint32_t) kTraceCap]; ++traceR_; }
        return n;
    }

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

    // ONE SHIFTER PER CHANNEL. A PsolaEngine holds a delay ring and a write
    // cursor, so sharing one across channels interleaves L and R in the same
    // ring and reconstructs each channel from the other's audio. Measured
    // before this was split: with identical input on both channels the outputs
    // differed by 0.76 peak, and passthrough was not even the delayed input.
    static constexpr int kMaxChannels = 2;
    echojay::PsolaEngine&       shifter (int ch = 0) noexcept       { return psola_[(size_t) juce::jlimit (0, kMaxChannels - 1, ch)]; }
    const echojay::PsolaEngine& shifter (int ch = 0) const noexcept { return psola_[(size_t) juce::jlimit (0, kMaxChannels - 1, ch)]; }

private:
    // Latency depends on voice_type (the lowest pitch the window must
    // represent), so it is recomputed whenever that changes and the host is
    // told. Reported honestly per spec §2.4 - a corrector that silently
    // misaligns a vocal against the track is worse than a slower one.
    void refreshLatency();

    // Apply a setting to every channel's shifter. They must stay identical in
    // configuration - only their audio differs.
    template <typename Fn> void forEachShifter (Fn&& fn)
    { for (auto& p : psola_) fn (p); }

    echojay::PitchEngine engine_;
    std::array<echojay::PsolaEngine, kMaxChannels> psola_;
    echojay::PitchCorrect correct_;
    echojay::F0JumpGate   f0Gate_;
    echojay::viz::PitchRibbonView ribbon_;
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
    std::atomic<uint64_t> keyFeedSelfId_ { 0 };
    std::atomic<bool>     keySelfGuard_ { true };    // ON by ruling (round 23, 5 Sep 2026): Sean's key is MANUAL, the flip is a measured no-op for him; see debugKeySelfGuard
    // The MANUAL reference field: only ever what a person entered (or its
    // 440 default). NEVER written from detection - the corrector's live
    // reference under auto lives in correct_ alone, so a state save cannot
    // launder a detected grid into a user setting (29 Aug 2026 defect).
    float retuneWasMs_ = 0.0f;   // pre-cap value from a clamped load; 0 = none
    std::atomic<float> manualRefHz_ { 440.0f };
    std::atomic<bool>  refManualByUser_ { false };
    // Carried ACROSS blocks. The slice before the first hop of a block
    // continues whatever the last hop of the previous block decided - resetting
    // these per block would make the first slice of every block use a stale or
    // zero target, which is a block-rate artefact of exactly the kind the
    // slicing exists to remove.
    float lastTarget_  = 0.0f;
    float lastShift_ = echojay::PsolaEngine::kNoShift;   // held with the target
    float lastHopF0_   = 0.0f;
    bool  lastHopVoiced_ = false;
    bool  lastCorrecting_ = false;
    int   ribbonAccum_ = 0;      // decimates ribbon pushes to ~30 Hz columns

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
    // A defaults write is not a hand on a knob either: writing the schema
    // defaults touches retune/flex/humanize/ignore_vibrato in turn, and each
    // would otherwise knock the display to custom - leaving correction_mode
    // contradicting its own advertised default of natural.
    void toCustomMode()
    { if (! applyingMode_ && ! writingDefaults_) modeIndex_.store (kCustom); }

    // applyMode/applyScale run inside setParamValue, whose caller builds its
    // own summary from id/value pairs and would report "correction_mode hard"
    // over six knobs that silently moved. They leave their detail here so
    // applyStructured can put it back into the applied line.
    juce::String pendingModeSummary_, pendingScaleSummary_;

    bool applyingMode_ = false;
    bool writingDefaults_ = false;

    std::atomic<int> modeIndex_ { kNatural };
    double               sampleRate_ = 48000.0;
    int                  latencyVoiceType_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedPitchProcessor)
};
