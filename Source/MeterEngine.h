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
    // Highest momentary / short-term reached over a capture. Only populated
    // on a frozen capture snapshot (see PluginProcessor::stopCapture); left at
    // -100 for live meter data, where a running max isn't meaningful.
    float momentaryMax = -100.0f;
    float shortTermMax = -100.0f;
    
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

    // Per-band crest factor (dB) — same sub (<120Hz) / mid (120Hz-5k) /
    // top (>5k) splits as the banded correlation, same windows as the global
    // crest. -1 = not yet measured (serialisation omits, absent=unavailable).
    float bandCrestSub = -1.0f;
    float bandCrestMid = -1.0f;
    float bandCrestTop = -1.0f;

    // Short-term true peak (dBTP) — max true peak over the same 3s window as
    // short-term LUFS. Drives PSR (= shortTermTruePeak - shortTerm).
    float shortTermTruePeak = -100.0f;

    // Inter-sample overs: count of contiguous runs above 0 dBTP (one event
    // per run, not per sample). Resets with the max-hold / on capture start.
    int oversCount = 0;
    
    // Stereo
    float width = 0.0f;
    float correlation = 0.0f;

    // Mid/Side energy ratio (side / mid). 0.0 = pure mid (mono),
    // higher values mean more side energy. Typical commercial material
    // sits roughly 0.2 to 0.8. Computed from the same HPF'd mid/side
    // accumulators that drive width, and held silent during quiet
    // buffers so it doesn't update on inaudible signal.
    float sideToMidRatio = 0.0f;

    // Banded correlation: phase relationship per frequency band.
    // sub  = below 120 Hz   (low-pass)
    // mid  = 120 Hz to 5 kHz (band-pass)
    // top  = above 5 kHz    (high-pass)
    // Same -1.0 to +1.0 scale as the overall correlation field.
    // Held during silence the same way the overall correlation is.
    float corrSub = 0.0f;
    float corrMid = 0.0f;
    float corrTop = 0.0f;
    
    // Goniometer — recent L/R sample pairs for vectorscope display
    static constexpr int gonioSize = 512;
    std::array<float, gonioSize> gonioL = {};
    std::array<float, gonioSize> gonioR = {};
    int gonioWritePos = 0;
    
    // Macro-band levels (dB, power-per-octave) — integrated from the RAW FFT
    // in computeSpectrum, separately from the display bins below (which carry
    // intentional visual shaping). Pink-referenced by construction: equal
    // energy per octave reads flat across bands. -120 = no data yet.
    // Order: sub, low, lowMid, mid, highMid, air.
    std::array<float, 6> macroBandDb = { -120,-120,-120,-120,-120,-120 };

    // Spectrum — 64 log-spaced bins from ~20 Hz to Nyquist
    static constexpr int numSpecBins = 64;
    std::array<float, 64> spectrum = {
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,
        -120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120,-120
    };

    // Rolling waveform — ring buffer of min/max peak pairs (~5s at ~86 pts/s)
    static constexpr int waveformSize = 512;
    struct WavePoint { float minVal = 0.0f; float maxVal = 0.0f; };
    std::array<WavePoint, waveformSize> waveform = {};
    int waveformWritePos = 0;
    int waveformCount = 0;  // how many valid points (up to waveformSize)

    // True when the plugin has not seen audible audio for the silence
    // timeout window (set in MeterEngine::prepare). When this is true,
    // all other meter values are stale snapshots from the last active
    // moment and should not be presented as current state.
    bool isSilent = true;
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

    // Serialise an arbitrary MeterData to the same JSON shape — used to send
    // a capture snapshot's averagedData with chat requests (the backend's
    // parseExtendedMeter accepts this exact blob). sampleRate drives the
    // macro-band bucket mapping only.
    static juce::String meterDataToJSON(const MeterData& d, double sampleRate);

    // ===== Spectrogram history (visualiser-only; never serialised) =====
    // Ring of display-bin frames decimated to ~25 fps (max-aggregated between
    // frames so transients survive), ~12s deep. Fixed allocation; the audio
    // thread pushes under dataMutex, the UI thread copies increments out.
    static constexpr int kSpecHistFrames = 300;

    // Copies up to maxFrames frames newer than sinceCounter into dest
    // (oldest→newest), returns the count and the new counter value. The
    // counter is monotonic so callers can fetch incrementally.
    int getSpectrogramFrames(int sinceCounter,
                             std::array<float, 64>* dest, int maxFrames,
                             int& counterOut) const;

    // ===== Visual-only FFT (SPECTRUM display detail; never serialised) =====
    // Additive display path: a second, larger FFT so the spectrum curve can
    // resolve Pro-Q-style peaks. Feeds ONLY getVisualSpectrum(). The 2048
    // analysis FFT (fftData → smoothedSpectrum/macroBands → backend JSON)
    // is a serialised data contract and is not touched by this path.
    static constexpr int kVisFftOrder = 12;                 // 2^12 = 4096
    static constexpr int kVisFftSize  = 1 << kVisFftOrder;
    static constexpr int kVisBins     = kVisFftSize / 2;    // usable magnitudes
    static constexpr int kVisHopSamples = 1024;             // ~43 FFTs/s at 44.1k

    // Copies the latest visual-FFT magnitudes (dB, floor -120) and the bin
    // width in Hz. Returns false until the first hop has been transformed
    // (dest is still valid: all floor).
    bool getVisualSpectrum(std::array<float, kVisBins>& dest, double& binHzOut) const;

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

    // Short-term true-peak window: per-100ms-block TP maxima, 30 blocks = 3s
    // (aligned with shortTermBlocks). Fed from the existing 4x detector —
    // no additional DSP.
    std::vector<float> stTpBlocks;
    float tp100msAccum = 0.0f;

    // Inter-sample overs event counter (contiguous-run, buffer-granular edge
    // detection on the existing detector output)
    bool tpOverRun  = false;
    int  oversEvents = 0;
    
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

    // Visual-only FFT state (see public constants above)
    juce::dsp::FFT visFft { kVisFftOrder };
    std::array<float, kVisFftSize * 2> visFftData = {};
    std::array<float, kVisFftSize>     visWindow  = {};
    std::vector<float> visAccumulator;                     // mono ring, kVisFftSize
    int  visWritePos = 0;
    int  visSamplesSinceFft = 0;
    std::array<float, kVisBins> visMagDb = {};             // dB, under dataMutex
    bool visReady = false;                                 // under dataMutex

    // Smoothed macro-band per-octave levels (see MeterData::macroBandDb)
    std::array<float, 6> smoothedMacroBands = { -120,-120,-120,-120,-120,-120 };

    // Spectrogram ring (see kSpecHistFrames). specAccum max-aggregates the
    // display bins between ~25fps frames; audio-thread-only except the ring
    // itself, which is written/read under dataMutex.
    std::array<std::array<float, 64>, kSpecHistFrames> specRing {};
    std::array<float, 64> specAccum {};
    int specWritePos = 0;
    int specFrameCount = 0;
    int specFrameCounter = 0;   // monotonic; survives resets
    int specAccumSamples = 0;
    int samplesPerSpecFrame = 1764;  // sampleRate/25, set in prepare()

    // Rolling waveform accumulator — downsample to ~86 pts/s (every 512 samples at 44.1k)
    static constexpr int kWaveDownsample = 512;
    float wfMinAccum = 0.0f, wfMaxAccum = 0.0f;
    int wfAccumCount = 0;
    void pushWaveformSamples(const float* left, const float* right, int numSamples);
    
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

    // ===== Banded correlation filters =====
    // Three band-limiting filters drive the corrSub / corrMid / corrTop
    // readings. Sub = LPF 120Hz, Mid = LPF 5kHz applied AFTER an HPF 120Hz
    // (i.e. band-pass via cascaded HP+LP for simplicity vs a real BPF),
    // Top = HPF 5kHz. All 2nd-order Butterworth. The bands intentionally
    // overlap by a sample's worth of phase at the crossover; we don't need
    // hard separation, we need a useful per-band reading.
    BiquadCoeffs corrSubLpf;     // LPF 120Hz for sub band
    BiquadCoeffs corrTopHpf;     // HPF 5kHz for top band
    BiquadCoeffs corrMidHpf;     // HPF 120Hz for mid band low-side
    BiquadCoeffs corrMidLpf;     // LPF 5kHz for mid band high-side
    // Filter state per band and channel (x1/x2/y1/y2 each, L and R)
    double subLpxL1=0, subLpxL2=0, subLpyL1=0, subLpyL2=0;
    double subLpxR1=0, subLpxR2=0, subLpyR1=0, subLpyR2=0;
    double topHpxL1=0, topHpxL2=0, topHpyL1=0, topHpyL2=0;
    double topHpxR1=0, topHpxR2=0, topHpyR1=0, topHpyR2=0;
    double midHpxL1=0, midHpxL2=0, midHpyL1=0, midHpyL2=0;
    double midHpxR1=0, midHpxR2=0, midHpyR1=0, midHpyR2=0;
    double midLpxL1=0, midLpxL2=0, midLpyL1=0, midLpyL2=0;
    double midLpxR1=0, midLpxR2=0, midLpyR1=0, midLpyR2=0;
    void computeBandedCorrCoeffs(double sr);

    // Display-smoothed banded correlation values. Same gating discipline
    // as the overall correlation: only updated when the band has audible
    // signal, else held at last meaningful value. Each band has its own
    // init flag because bands fill at different times on real material
    // (a kick-only track has nothing in the top band).
    float displayCorrSub = 0.0f;
    float displayCorrMid = 0.0f;
    float displayCorrTop = 0.0f;
    bool subInit = false;
    bool midInit = false;
    bool topInit = false;

    // Per-band crest accumulators — fed from the SAME band samples the
    // banded-correlation loop already computes (no additional filtering).
    // RMS reuses the correlation energy sums; only the per-sample abs-max
    // peak is new. Same constants as the global crest: 0.5s RMS EMA, 3s
    // peak decay, hold-last-value during silence (-1 until first valid).
    double bandSqSub = 0, bandSqMid = 0, bandSqTop = 0;
    float  bandPeakSub = 0, bandPeakMid = 0, bandPeakTop = 0;
    float  lastBandCrestSub = -1.0f;
    float  lastBandCrestMid = -1.0f;
    float  lastBandCrestTop = -1.0f;

    // Display-smoothed side/mid ratio. Reuses the same EMA-on-the-scalar
    // pattern as width (smoothing the ratio rather than the components,
    // so vocal reverb tails don't produce wild swings).
    float displaySideToMid = 0.0f;
    bool sideToMidInit = false;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MeterEngine)

    // ===== Silence detection (for stale-meter prevention) =====
    // Counts consecutive samples seen below kSilenceThreshold. When the
    // count exceeds silenceTimeoutSamples (set to 500ms in prepare()),
    // the engine reports isSilent=true so the chat layer can avoid
    // sending stale meter values to the model. Reset to 0 whenever any
    // sample exceeds the threshold.
    std::atomic<int> silentSampleCount {0};
    int silenceTimeoutSamples = 24000; // overwritten in prepare()
    static constexpr float kSilenceThreshold = 0.0001f; // ~-80dBFS

};
