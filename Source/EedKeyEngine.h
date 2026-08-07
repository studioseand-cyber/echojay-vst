/*
    EedKeyEngine.h  —  the musical key detector behind "EchoJay Key Detector"
    (KEY_DETECTOR_SPEC.md §2). Deliberately JUCE-free, like EqEngine/EqMove,
    so the whole detection pipeline unit-tests under plain g++
    (test/key_engine_test.cpp).

    This is a READER: it never touches audio. The processor taps the stream
    into pushBlock() (audio thread, lock-free) and a background thread calls
    update(), which drains the tap and advances the analysis; results come out
    of getReading().

    THE PIPELINE, in the spec's descending order of impact on accuracy — the
    published techniques behind the accurate commercial detectors, not any
    product's internals:

      1. TUNING ESTIMATION FIRST. The wrapped constant-Q energy is examined at
         3 sub-bins per semitone; the circular mean of the sub-bin phase is the
         track's deviation from A=440 in cents. Every later step folds its bins
         through that offset, and the offset itself ships as a first-class
         output (detected_tuning_hz) — a track 30 cents sharp would otherwise
         smear across pitch-class bins and quietly wreck everything downstream.
      2. CONSTANT-Q, not linear FFT: log-spaced bins aligned to semitones
         (36/octave), evaluated directly per bin so the bass has the same pitch
         resolution as the top. This is the single biggest jump over an FFT
         chromagram, whose bins are far too coarse exactly where the root lives.
      3. HARMONIC/PERCUSSIVE SEPARATION (median-filtering HPSS, Fitzgerald
         2010): median across time suppresses broadband transients, median
         across frequency suppresses sustained tones; a Wiener mask keeps the
         harmonic part. This is what rescues dense drum-heavy mixes.
      4. SPECTRAL WHITENING + HARMONIC SUMMATION: log-magnitude minus its local
         (±half-octave) envelope flattens timbre; each pitch's harmonics
         (h = 1..4, 1/h weights) are then folded onto its fundamental so a bass
         note and its overtones reinforce ONE class instead of lighting several.
      5. SEGMENT CHROMA + VITERBI: a chroma vector per ~0.5 s segment, then a
         Viterbi pass over the 24 key states with a self-transition bias —
         which is what stops relative-major/minor flip-flopping while letting a
         genuine modulation register as a change.
      6. KEY PROFILES: Temperley's revision of Krumhansl-Schmuckler
         ("What's Key for Key?", 1999), which outperforms the raw K-S weights
         on popular material. Confidence = winner normalised against the
         runner-up, so "C major 0.90 / A minor 0.88" honestly reports LOW.
      7. HYSTERESIS + HOLD for the continuous mode; the primary interaction is
         the COMMITTED pass (arm, listen for window_s, commit, hold).

    THREADING CONTRACT. pushBlock() is the only audio-thread entry: bounded
    writes + one release store, no locks, no allocation. update() and every
    setter/getter run on ONE consumer side (the processor's analysis thread
    and/or the message thread); getReading()/getLiveChroma() take a mutex that
    the audio thread never sees.
*/

#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace echojay
{

// ---------------------------------------------------------------------------
// One detection result. POD-ish and self-contained so it can be copied out
// under the mutex and formatted anywhere (editor, AI feed) without touching
// the engine again.
// ---------------------------------------------------------------------------
struct KeyReading
{
    bool  valid       = false;   // false until a pass has produced a key
    bool  committed   = false;   // true = from an ANALYSE pass (vs continuous)
    int   root        = 0;       // 0..11 = C..B
    bool  minor       = false;
    float confidence  = 0.0f;    // 0..1, margin-normalised (close call = low)
    float tuningHz    = 440.0f;  // detected (or the hint, when auto_tuning off)
    float tuningCents = 0.0f;    // the same offset, in cents from A=440
    float rootHz      = 0.0f;    // the root's fundamental, tuning-corrected
    int   rootMidi    = 0;       // the octave rootHz was reported in
    float analysedSeconds = 0.0f;

    std::array<float, 12> chroma {};    // tuned pitch-class profile, max = 1

    struct Alt { int root = 0; bool minor = false; float score = 0.0f; };
    std::array<Alt, 3> alternates {};
    int numAlternates = 0;

    std::array<float, 24> score {};     // aggregate correlation per key
                                        // (index = root + (minor ? 12 : 0))
};

// ---------------------------------------------------------------------------
// What the engine is DOING right now — the UI's honesty contract. A user must
// always be able to tell "working on it" from "broken": an armed pass with no
// audio arriving says so, a running pass shows its progress, and a pass that
// completed over nothing tonal is reported rather than silently ignored.
// ---------------------------------------------------------------------------
struct KeyActivity
{
    bool  collecting       = false;  // a committed pass is armed/gathering
    bool  waitingForSignal = false;  // armed, but no (or silent) audio is
                                     // arriving — stopped transport or a
                                     // silent channel; the window only starts
                                     // counting once signal appears
    float progress         = 0.0f;   // 0..1 of the committed window
    float gatheredSeconds  = 0.0f;
    float windowSeconds    = 0.0f;
    bool  lastPassEmpty    = false;  // the most recent completed pass found
                                     // nothing tonal (silence / noise floor);
                                     // the previous reading was KEPT
    uint32_t passesCompleted = 0;
};

// ---------------------------------------------------------------------------
class KeyEngine
{
public:
    KeyEngine();

    // ---- ranges the ParamSchema advertises (single source of truth) -------
    static constexpr float kMinWindowS   = 1.0f,   kMaxWindowS   = 30.0f;
    static constexpr float kDefWindowS   = 10.0f;
    static constexpr float kMinTuningHz  = 415.0f, kMaxTuningHz  = 465.0f;
    static constexpr float kMinLowHz     = 20.0f,  kMaxLowHz     = 500.0f;
    static constexpr float kDefLowHz     = 80.0f;
    static constexpr float kMinHighHz    = 1000.0f, kMaxHighHz   = 12000.0f;
    static constexpr float kDefHighHz    = 5000.0f;

    // mode_lock choice indices — the schema's choices list mirrors this order.
    enum ModeLock { kLockAuto = 0, kLockMajor = 1, kLockMinor = 2 };

    // ---- lifecycle --------------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);
    void reset();                        // clears tap, accumulation and reading

    // ---- audio thread -----------------------------------------------------
    // Observes only. r may be null (mono).
    void pushBlock (const float* l, const float* r, int n) noexcept;

    // ---- control (message/analysis thread) --------------------------------
    void  setWindowSeconds (float s)      { windowS_.store (clampf (s, kMinWindowS, kMaxWindowS)); }
    float getWindowSeconds() const        { return windowS_.load(); }
    void  setContinuous (bool b)          { continuous_.store (b); }
    bool  getContinuous() const           { return continuous_.load(); }
    void  setSensitivity (float pct)      { sensitivity_.store (clampf (pct, 0.0f, 100.0f)); }
    float getSensitivity() const          { return sensitivity_.load(); }
    void  setTuningHint (float hz)        { tuningHint_.store (clampf (hz, kMinTuningHz, kMaxTuningHz)); }
    float getTuningHint() const           { return tuningHint_.load(); }
    void  setAutoTuning (bool b)          { autoTuning_.store (b); }
    bool  getAutoTuning() const           { return autoTuning_.load(); }
    void  setModeLock (int m)             { modeLock_.store (m < 0 ? 0 : (m > 2 ? 2 : m)); }
    int   getModeLock() const             { return modeLock_.load(); }
    void  setHold (bool b)                { hold_.store (b); }
    bool  getHold() const                 { return hold_.load(); }
    void  setHpss (bool b)                { hpss_.store (b); }
    bool  getHpss() const                 { return hpss_.load(); }
    void  setLowHz (float hz)             { lowHz_.store (clampf (hz, kMinLowHz, kMaxLowHz)); }
    float getLowHz() const                { return lowHz_.load(); }
    void  setHighHz (float hz)            { highHz_.store (clampf (hz, kMinHighHz, kMaxHighHz)); }
    float getHighHz() const               { return highHz_.load(); }

    // Arm a committed pass: the NEXT windowSeconds of PLAYBACK are gathered
    // (silence before the signal starts does not count), analysed once, and
    // the result holds until re-armed (or reset).
    //
    // TRIGGER SEMANTICS. These are safe from any thread: they set request
    // flags plus the immediately-visible state (collecting_, the cleared
    // reading) and the consumer-side counter surgery happens at the TOP of
    // the next update() — the only thread allowed to touch that state. A
    // host that wants the trigger to take effect NOW (it should — a manual
    // trigger that waits for a timer reads as a broken device) kicks its
    // worker thread (EedKeyWorker::notify()) right after calling these.
    void  startAnalysis();
    void  cancelAnalysis();
    bool  isCollecting() const            { return collecting_.load(); }
    float collectionProgress() const;     // 0..1 while collecting

    // Clear gathered audio and the held reading (the `reset` param). The
    // reading clears IMMEDIATELY (a stale key displayed as current is worse
    // than none); the accumulation clears on the next update().
    void  clearAccumulation();

    // What is happening right now, for the UI. Cheap; any thread.
    KeyActivity getActivity() const;

    // Drain the tap and advance the state machine; runs the actual analysis
    // when a committed pass completes (or on the continuous cadence). Call
    // from ONE background/message thread, a few times a second. Returns true
    // when the published reading changed.
    bool  update();

    // ---- results ----------------------------------------------------------
    KeyReading getReading() const;

    // Display-only: the most recent segment-level chroma (tuned, normalised),
    // refreshed by update() even mid-collection so a UI can animate. Falls
    // back to the committed reading's chroma when nothing newer exists.
    void getLiveChroma (float out[12]) const;

    // "F#", "C", …
    static const char* pitchClassName (int pc);
    // "F# minor" — formatted into a caller buffer (>= 16 chars).
    static void keyName (int root, bool minor, char* buf, int bufLen);

    // The analysis sample rate after internal decimation — exposed for tests.
    double analysisRate() const           { return fsA_; }

private:
    static float clampf (float v, float lo, float hi)
    { return v < lo ? lo : (v > hi ? hi : v); }

    // ---- the audio-thread tap ---------------------------------------------
    // Mono downmix -> 4th-order lowpass -> decimate -> lock-free ring. All
    // audio-thread state lives in this block and is touched nowhere else
    // (prepare()/reset() run with audio stopped).
    static constexpr int kRingSize = 1 << 19;   // 524288 ≈ 32 s at 16 kHz
    static constexpr int kRingMask = kRingSize - 1;

    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        inline float process (float x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float) y;
        }
        void makeLowpass (double fs, double fc, double q);
        void resetState() { z1 = z2 = 0; }
    };

    std::array<float, (std::size_t) kRingSize> ring_ {};
    std::atomic<uint64_t> ringWrite_ { 0 };     // absolute decimated-sample count
    Biquad aa1_, aa2_;                          // anti-alias before decimation
    int    decimFactor_ = 3;
    int    decimPhase_  = 0;

    // ---- consumer-side accumulation ---------------------------------------
    std::vector<float> history_;                // circular, consumer-side copy
    uint64_t histCount_ = 0;                    // absolute count of samples in history_
    uint64_t ringRead_  = 0;

    void drainRing();
    int  copyRecent (std::vector<float>& dest, int n) const;   // newest n, chronological
    int  copyEnding (std::vector<float>& dest, uint64_t endAbs, int n) const;

    // ---- the analysis passes ----------------------------------------------
    struct PassResult
    {
        bool ok = false;
        int  key = 0;                 // 0..23
        float conf = 0.0f;
        float cents = 0.0f;
        float rootHz = 0.0f;
        int   rootMidi = 0;
        std::array<float, 12> chroma {};
        std::array<float, 24> score {};
    };

    // The full pipeline over n samples ending at endAbs. hpssOn is a
    // parameter (not read from the atomic) so tests can A/B the separation
    // on one input.
    PassResult analyseWindow (int n, bool hpssOn, uint64_t endAbs);

    // Stages, split for testability and reuse.
    void hpssSeparate (std::vector<float>& x) const;            // in place
    void computeSegmentSalience (const std::vector<float>& x,
                                 std::vector<std::array<float, 36>>& wrappedPerSeg,
                                 std::vector<float>& aggSalience,
                                 int& bLoOut, int& bHiOut) const;
    static float estimateTuningSubbins (const std::vector<std::array<float, 36>>& wrapped);
    static void foldChroma (const std::array<float, 36>& wrapped, float offSub,
                            std::array<float, 12>& chromaOut);

    void publish (const PassResult& r, bool committed, float seconds);

    // ---- state machine ----------------------------------------------------
    std::atomic<float> windowS_     { kDefWindowS };
    std::atomic<bool>  continuous_  { false };
    std::atomic<float> sensitivity_ { 50.0f };
    std::atomic<float> tuningHint_  { 440.0f };
    std::atomic<bool>  autoTuning_  { true };
    std::atomic<int>   modeLock_    { kLockAuto };
    std::atomic<bool>  hold_        { false };
    std::atomic<bool>  hpss_        { true };
    std::atomic<float> lowHz_       { kDefLowHz };
    std::atomic<float> highHz_      { kDefHighHz };

    std::atomic<bool>  collecting_  { false };

    // Trigger requests (any thread -> consumed at the top of update()).
    std::atomic<bool>  armRequest_   { false };
    std::atomic<bool>  clearRequest_ { false };
    // The tap position captured AT the arm request, so the window starts at
    // the sample ANALYSE was pressed on — not wherever the worker's next
    // drain happened to leave the counters.
    std::atomic<uint64_t> armRingPos_ { 0 };

    // Activity flags published by update() for getActivity().
    std::atomic<bool>     waitingForSignal_ { false };
    std::atomic<bool>     lastPassEmpty_    { false };
    std::atomic<uint32_t> passesCompleted_  { 0 };

    // CONSUMER-THREAD-ONLY counters. These four move together: every one is
    // relative to histCount_, so zeroing histCount_ without the rest leaves a
    // stale high-water mark the counter has to regrow past — which is exactly
    // the "dead for a minute after RESET" bug. resetCounters() is the only
    // way any of them is cleared.
    uint64_t armedAt_ = 0;                      // histCount_ when armed
    uint64_t lastContinuousAt_ = 0;
    uint64_t lastLiveAt_ = 0;                   // last live-chroma refresh
    bool     signalSeen_ = false;               // this pass has heard signal

    void resetCounters();

    double fs_  = 48000.0;
    double fsA_ = 16000.0;

    // ---- published results -------------------------------------------------
    mutable std::mutex   readingMutex_;
    KeyReading           reading_;
    std::array<float,12> liveChroma_ {};
    bool                 haveLiveChroma_ = false;

    std::vector<float> work_;                   // scratch for a pass
};

} // namespace echojay
