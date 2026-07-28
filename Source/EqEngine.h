/*
    EqEngine.h  —  EchoJay built-in surgical EQ, headless DSP core.

    Design notes
    ------------
    * Deliberately free of any JUCE dependency so the DSP can be unit-tested
      in isolation and reused anywhere. It operates on raw float buffers and
      std types only. The JUCE-facing AudioProcessor wrapper + editor live in
      separate files and drive this engine.

    * Filters are TPT state-variable (Cytomic/Zavalishin topology), NOT
      Direct-Form biquads. SVF holds its exact frequency and Q at high
      resonance and near Nyquist, which is exactly what surgical work needs.

    * Parameter updates are single-writer (message thread) / single-reader
      (audio thread), published via a seqlock so the audio thread never tears
      a half-written BandSpec and never blocks. The audio thread smooths the
      published targets at block rate and recomputes coefficients per block.

    * The magnitude response used by the UI is computed analytically from the
      SVF state-space (exact), so the on-screen curve matches the audio path.

    Static bands are implemented here. Dynamic (threshold-driven) bands reuse
    the same BandSpec fields and are layered on in a follow-up.
*/

#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <complex>
#include <vector>

namespace echojay
{

// ---------------------------------------------------------------------------
enum class BandType : int
{
    Bell = 0,
    LowShelf,
    HighShelf,
    Notch,
    HighPass,
    LowPass,
    NumTypes
};

// A single band's public specification, in real-world units.
// This is the exact struct the AI apply path and the UI both write.
struct BandSpec
{
    bool     enabled  = false;
    BandType type     = BandType::Bell;
    float    freqHz   = 1000.0f;
    float    gainDb   = 0.0f;     // bell / shelves only
    float    q        = 0.707f;   // bell / notch / HP / LP resonance
    int      slopeDbPerOct = 12;  // HP / LP only: 6,12,18,...,96 (multiple of 6)

    // Dynamic (threshold-driven) extension. When dynamic == false the band is
    // a plain static filter and these are ignored.
    bool     dynamic     = false;
    float    thresholdDb = 0.0f;  // detector threshold
    float    rangeDb     = 0.0f;  // max additional gain applied when triggered
    float    attackMs    = 10.0f;
    float    releaseMs   = 100.0f;

    bool operator== (const BandSpec& o) const
    {
        return enabled == o.enabled && type == o.type
            && freqHz == o.freqHz && gainDb == o.gainDb && q == o.q
            && slopeDbPerOct == o.slopeDbPerOct
            && dynamic == o.dynamic && thresholdDb == o.thresholdDb
            && rangeDb == o.rangeDb && attackMs == o.attackMs
            && releaseMs == o.releaseMs;
    }
    bool operator!= (const BandSpec& o) const { return ! (*this == o); }
};

// ---------------------------------------------------------------------------
class EqEngine
{
public:
    static constexpr int kMaxBands    = 24;
    static constexpr int kMaxChannels = 2;

    EqEngine() = default;

    // ---- lifecycle (audio thread / setup thread) --------------------------
    void  prepare (double sampleRate, int maxBlockSize, int numChannels);
    void  reset();

    // ---- parameter access -------------------------------------------------
    // setBand / setBands are called from the message thread. They publish
    // targets via the seqlock; the audio thread picks them up on its next
    // process() call. Safe to call while audio is running.
    void  setBand  (int index, const BandSpec& spec);
    void  setBands (const BandSpec* specs, int count);   // publishes all at once
    BandSpec getBand (int index) const;                  // last published target

    void  setBypassed (bool shouldBypass) noexcept { bypassed_.store (shouldBypass, std::memory_order_relaxed); }
    bool  isBypassed() const noexcept { return bypassed_.load (std::memory_order_relaxed); }

    // Solo one band for auditioning (‑1 == no solo). When soloed, only that
    // band processes. Message thread; picked up on next block.
    void  setSoloBand (int index) noexcept { soloBand_.store (index, std::memory_order_relaxed); }
    int   getSoloBand() const noexcept { return soloBand_.load (std::memory_order_relaxed); }

    static constexpr int kNumBands = kMaxBands;

    // ---- audio (audio thread) --------------------------------------------
    // In-place processing of an array of channel pointers.
    void  process (float* const* channels, int numChannels, int numSamples) noexcept;

    // ---- analysis (message/UI thread) ------------------------------------
    // Fill magsDb[i] with the total EQ magnitude response (dB) at freqsHz[i],
    // using the most recently published targets. Exact w.r.t. the SVF path.
    void  getMagnitudeResponse (const float* freqsHz, float* magsDb, int n) const;

    // Response of a single band only (for the UI's per-band curves).
    void  getBandMagnitudeResponse (int index, const float* freqsHz, float* magsDb, int n) const;

    double getSampleRate() const noexcept { return sampleRate_; }

private:
    // -- SVF coefficient set (mix form) ------------------------------------
    struct Coeffs
    {
        float g = 0, k = 1, a1 = 1, a2 = 0, a3 = 0;
        float m0 = 1, m1 = 0, m2 = 0;
        bool  passthrough = true; // disabled / unity band → skip
    };

    // one biquad-equivalent SVF stage's runtime state, per channel
    struct StageState { float ic1 = 0, ic2 = 0; };

    // A band expands to one or more cascaded SVF stages (HP/LP with steep
    // slopes cascade identical 12 dB/oct sections; everything else is 1 stage).
    struct BandRuntime
    {
        int    numStages = 0;
        Coeffs coeffs[8];                              // up to 96 dB/oct (8×12)
        StageState state[8][kMaxChannels];
        // smoothed real-unit params actually realised this block:
        float  curFreq = 1000, curGain = 0, curQ = 0.707f;
        bool   curEnabled = false;
        BandType curType = BandType::Bell;
        int    curSlope = 12;
    };

    static Coeffs computeCoeffs (BandType type, double fs, float freqHz,
                                 float gainDb, float q) noexcept;
    static int    stagesForSlope (int slopeDbPerOct) noexcept;
    static float  processStage (Coeffs& c, StageState& s, float x) noexcept;
    static std::complex<double> stageResponse (const Coeffs& c, double omega) noexcept;

    void pullTargetsIfChanged() noexcept;   // audio thread: seqlock read + smoothing setup
    void snapshotForAnalysis (BandSpec out[kMaxBands]) const;

    // -- seqlock published targets -----------------------------------------
    // Writer: message thread. Reader: audio thread + analysis.
    std::atomic<uint32_t> seq_ { 0 };       // even = stable, odd = write in progress
    BandSpec targets_[kMaxBands];           // guarded by seq_
    std::atomic<uint32_t> dirtyPulse_ { 0 };// bumped on every publish

    // audio-thread private copy of most-recent targets
    BandSpec   activeTargets_[kMaxBands];
    uint32_t   lastSeenPulse_ = 0xffffffff;

    BandRuntime bands_[kMaxBands];

    double sampleRate_ = 44100.0;
    int    maxBlock_   = 512;
    int    numCh_      = 2;

    float  smoothingCoeff_ = 0.0f;          // one-pole per-block smoothing
    std::atomic<bool> bypassed_ { false };
    std::atomic<int>  soloBand_ { -1 };

    // scratch for analysis snapshot (mutable: const analysis methods)
    mutable BandSpec analysisScratch_[kMaxBands];
};

} // namespace echojay
