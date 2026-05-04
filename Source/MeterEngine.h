#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include <mutex>

// K-weighting filter coefficients for LUFS (ITU-R BS.1770-4)
struct BiquadCoeffs {
    double b0, b1, b2, a1, a2;
};

struct MeterData {
    // Loudness (LUFS)
    float momentary = -100.0f;
    float shortTerm = -100.0f;
    float integrated = -100.0f;
    float loudnessRange = 0.0f;
    
    // Levels
    float rmsL = -100.0f, rmsR = -100.0f;
    float peakL = -100.0f, peakR = -100.0f;
    float truePeakL = -100.0f, truePeakR = -100.0f;
    float truePeakMaxL = -100.0f, truePeakMaxR = -100.0f;
    float truePeakBarL = -100.0f, truePeakBarR = -100.0f; // decaying value for bar display
    float peakMaxL = -100.0f, peakMaxR = -100.0f;
    
    // Dynamics
    float crestFactor = 0.0f;
    float dcOffset = 0.0f;
    
    // Stereo
    float width = 0.0f;
    float correlation = 0.0f;
    
    // Goniometer — recent L/R sample pairs for vectorscope display
    static constexpr int gonioSize = 512;
    std::array<float, gonioSize> gonioL = {};
    std::array<float, gonioSize> gonioR = {};
    int gonioWritePos = 0;
    
    // Spectrum — 64 log-spaced bins from ~20 Hz to Nyquist
    static constexpr int numSpecBins = 64;
    std::array<float, 64> spectrum = {
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120
    };
};

class MeterEngine
{
public:
    MeterEngine();
    ~MeterEngine();
    
    void prepare(double sampleRate, int samplesPerBlock);
    
    // Request a reset. Thread-safe. The actual reset happens on the audio
    // thread at the start of the next processBlock — this avoids a race
    // condition where the message thread clears vectors while the audio
    // thread is mid-processBlock writing to them. Caused intermittent garbage
    // measurements (e.g. -8 LUFS for a -27 LUFS source) when capture was
    // started during the wrong audio buffer.
    void reset();
    void processBlock(const float* left, const float* right, int numSamples);
    
    // Thread-safe access to current meter values
    MeterData getMeterData() const;
    
    // Reset integrated LUFS measurement
    void resetIntegrated();
    
    // Get JSON string of all meter data for the WebView
    juce::String getMeterDataJSON() const;
    
private:
    double currentSampleRate = 44100.0;
    
    // K-weighting filters (stage 1: high shelf, stage 2: high pass)
    BiquadCoeffs kStage1, kStage2;
    // Filter state per channel
    double s1x1L = 0, s1x2L = 0, s1y1L = 0, s1y2L = 0;
    double s1x1R = 0, s1x2R = 0, s1y1R = 0, s1y2R = 0;
    double s2x1L = 0, s2x2L = 0, s2y1L = 0, s2y2L = 0;
    double s2x1R = 0, s2x2R = 0, s2y1R = 0, s2y2R = 0;
    
    // LUFS gating blocks (100ms blocks at 10ms overlap)
    struct LufsBlock {
        double power;
        double loudness;
        int64_t timeMs;
    };
    std::vector<LufsBlock> momentaryBlocks;  // 400ms window
    std::vector<LufsBlock> shortTermBlocks;  // 3s window
    std::vector<LufsBlock> allBlocks;        // all blocks for integrated
    std::vector<double> shortTermHistory;   // short-term loudness values for LRA (3s windows)
    
    // Ring buffer for instant momentary LUFS (400ms of K-weighted power per sample)
    std::vector<double> kPowerRing;          // sum of L²+R² per sample, K-weighted
    int kPowerRingSize = 0;                  // 400ms worth of samples
    int kPowerWritePos = 0;
    double kPowerSum = 0;                    // running sum for fast average
    int kPowerCount = 0;                     // how many valid samples in ring
    
    // K-weighted sample accumulator
    std::vector<double> kWeightedL, kWeightedR;
    int blockSampleCount = 0;
    int samplesPerBlock100ms = 0;
    
    // True peak (4x oversampling)
    void computeTruePeak(const float* samples, int numSamples, float& truePeak);
    
    // FFT spectrum analysis (2048-point -> 64 log-spaced bins)
    static constexpr int fftOrder = 11;                    // 2^11 = 2048
    static constexpr int fftSize  = 1 << fftOrder;         // 2048
    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize * 2> fftData = {};           // input: real samples in first half; output: magnitudes
    std::array<float, fftSize>     fftWindow = {};
    std::vector<float> fftAccumulator;                     // ring buffer for mono samples
    int fftWritePos = 0;
    std::array<float, 64> smoothedSpectrum = {
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120
    };

    void computeSpectrum(const float* left, const float* right, int numSamples);
    
    // Apply K-weighting biquad filter
    double applyBiquad(double input, const BiquadCoeffs& c,
                       double& x1, double& x2, double& y1, double& y2);
    
    // Compute K-weighting coefficients for given sample rate
    void computeKWeightingCoeffs(double sr);
    
    // Current meter data (protected by mutex)
    mutable std::mutex dataMutex;
    MeterData data;
    
    // Reset coordination: message thread sets this true via reset(), audio
    // thread checks it at top of processBlock and runs resetState() if set,
    // then clears it. Avoids data race on the unprotected vectors below.
    std::atomic<bool> pendingReset { false };
    void resetState();   // does the actual reset work — called from audio thread
    
    // Running accumulators
    double sumSqL = 0, sumSqR = 0;
    double sumL = 0, sumR = 0;
    float currentPeakL = 0, currentPeakR = 0;
    float currentTpL = 0, currentTpR = 0;
    float displayTpL = 0, displayTpR = 0; // decaying for bar display
    int sampleCount = 0;
    
    // Stereo accumulators
    double sumMid = 0, sumSide = 0;
    double sumCorr = 0, sumEnergyL = 0, sumEnergyR = 0;
    
    // Display-smoothed width and correlation values. Computed by smoothing the
    // FINAL ratio (not the components — see comment in processBlock for why).
    // 1.5s EMA gives a calmer, more usable reading on signals with intermittent
    // stereo content like vocals with reverb tails.
    float displayWidth = 0.0f;
    float displayCorr = 0.0f;
    bool displayStereoInit = false;
    
    // Last meaningful crest reading, held across silent buffers so the meter
    // doesn't grow to infinity when playback stops (RMS decays to 0 while peak
    // holds, ratio explodes).
    float lastCrest = 0.0f;
    
    // High-pass filter for width calculation (~300Hz, 2nd order)
    // Prevents bass from collapsing width reading
    BiquadCoeffs widthHpf;
    double whpx1L = 0, whpx2L = 0, whpy1L = 0, whpy2L = 0;
    double whpx1R = 0, whpx2R = 0, whpy1R = 0, whpy2R = 0;
    void computeWidthHpfCoeffs(double sr);
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MeterEngine)
};
