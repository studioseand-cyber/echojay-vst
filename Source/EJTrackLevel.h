#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace echojay
{

/** THE LEVEL THE PLUGIN SENDS WITH A BUILD (COMP_PROFILE_SPEC_v1 section 5).

    ```json
    "track_level": { "loud_rms_dbfs": -18.4, "loud_peak_dbfs": -6.2,
                     "window": "400ms_p95", "heard_s": 90 }
    ```

    `loud_rms_dbfs` is the 95th percentile of 400 ms RMS over what has been HEARD, measured on the track
    PRE-CHAIN. Under 20 s heard there is no answer and the field is null: the server then computes no threshold
    and falls back to today's behaviour.

    WHY A PERCENTILE AND NOT A MEAN OR A MAX. The profile's `eff_threshold_dbfs` is the input level at which the
    compressor reaches 1 dB of gain reduction, so what the server needs is the level of the material that is
    SUPPOSED to be compressed - the loud phrases - not the average of a take that is mostly silence, and not the
    single loudest 400 ms, which is one breath away from being an outlier. The 95th percentile of the heard
    windows is the loud phrases, and it is stable from take to take.

    WHY A HISTOGRAM. A percentile needs the distribution, and this runs on the AUDIO THREAD: no allocation, no
    sorting, no growing vector. 0.25 dB bins from -96 to 0 dBFS is 385 counters, exact to a quarter of a dB,
    which is far inside the 1 dB the acceptance test allows. Windows below the silence gate never enter it, so a
    take that is 80% silence does not drag the percentile down.

    THE UNITS ARE PLAIN RMS dBFS, which is what the profile means by `level_ref: "sine_rms_dbfs"`: the sweep
    reports its sine's RMS in dBFS (a full-scale sine reads -3.01), and this reports the programme's RMS in dBFS
    the same way, so the two are the same reference and the subtraction in section 6 is meaningful.

    Pure and header-only, so a guard drives it on a signal whose levels it chose. */
class TrackLevel
{
public:
    static constexpr double kWindowMs      = 400.0;   // spec section 5
    static constexpr float  kPercentile    = 0.95f;   // "95th percentile of 400 ms RMS"
    // 2 Oct 2026 RULING (Sean): THREE SECONDS, NOT TWENTY. The 20 s floor came from wanting a stable percentile,
    // and it cost more than it bought: a build on a track that had played a few seconds sent no track_level at all,
    // so the server computed no threshold and fell back - on exactly the turns someone is auditioning a short
    // phrase. Three seconds of audio ABOVE THE GATE is enough to place a vocal within the 1 dB the acceptance test
    // allows, and heard_s rides the payload so the server can weigh it.
    static constexpr float  kMinHeardS     = 3.0f;
    // ...AND A CLIP SHORTER THAN THAT IS STILL AN ANSWER, once it has played through. Waiting for a floor that
    // will never arrive is the same failure as the 20 s one. "Played through" is measured on the SAMPLE CLOCK -
    // audio that stopped arriving - not on wall clock, so it is deterministic and a guard can drive it: when no
    // new above-gate window has closed for this long, what was heard is what there is.
    static constexpr float  kSettledAfterS = 2.0f;
    static constexpr float  kSilenceGateDb = -70.0f;  // a window quieter than this is not programme
    static constexpr float  kBinDb         = 0.25f;
    static constexpr float  kLoDb          = -96.0f;
    static constexpr int    kBins          = (int) ((0.0f - kLoDb) / kBinDb) + 1;   // 385

    struct Reading
    {
        bool  valid = false;          // false => send null
        float loudRmsDbfs  = 0.0f;
        float loudPeakDbfs = 0.0f;
        float heardSeconds = 0.0f;
        int   windows = 0;            // how many 400 ms windows entered the histogram
    };

    void prepare (double sampleRate)
    {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        windowSamples_ = juce::jmax (1, (int) std::lround (sr_ * kWindowMs * 0.001));
        reset();
    }

    void reset() noexcept
    {
        bins_.fill (0);
        binSumDb_.fill (0.0);
        total_ = 0;
        sumSq_ = 0.0;
        inWindow_ = 0;
        windowPeak_ = 0.0f;
        heardSamples_ = 0;
        sinceHeardGrew_ = 0;
        peakBins_.fill (0);
        peakSumDb_.fill (0.0);
    }

    /** One block, mono or stereo, from the PRE-CHAIN tap. Audio thread: no allocation, no locks. */
    void push (const float* left, const float* right, int n) noexcept
    {
        if (left == nullptr || n <= 0) return;
        for (int i = 0; i < n; ++i)
        {
            const float l = left[i];
            const float s = (right != nullptr) ? 0.5f * (l + right[i]) : l;
            sumSq_ += (double) s * (double) s;
            windowPeak_ = juce::jmax (windowPeak_, std::abs (s));
            if (++inWindow_ >= windowSamples_)
            {
                const auto before = heardSamples_;
                closeWindow();
                // The sample clock since the last window that COUNTED. A window below the gate is not heard
                // audio, so it advances this instead of resetting it: silence cannot keep a reading pending.
                if (heardSamples_ > before) sinceHeardGrew_ = 0;
                else                        sinceHeardGrew_ += (juce::int64) windowSamples_;
                inWindow_ = 0; sumSq_ = 0.0; windowPeak_ = 0.0f;
            }
        }
    }

    /** HEARD time is the audio that entered a window above the gate, not wall clock: a stopped transport simply
        stops adding to it, which is the same rule the calibration loop's settle uses. */
    float heardSeconds() const noexcept { return (float) ((double) heardSamples_ / sr_); }

    Reading read() const noexcept
    {
        Reading r;
        r.heardSeconds = heardSeconds();
        r.windows = total_;
        // NULL ONLY WHEN NOTHING ABOVE THE GATE WAS HEARD (2 Oct 2026 ruling). Otherwise: three seconds, or
        // whatever was heard once the material has stopped arriving. Silence alone can never satisfy either,
        // because a window below the gate never enters total_ and never advances heardSamples_.
        if (total_ <= 0 || r.heardSeconds <= 0.0f) return r;        // valid stays false => null on the wire
        const bool enough  = r.heardSeconds >= kMinHeardS;
        const bool settled = (double) sinceHeardGrew_ >= (double) kSettledAfterS * sr_;
        if (! enough && ! settled) return r;                       // still filling, and still arriving
        // The percentile by counting up from the quiet end: the first bin at or past 95% of the windows. Both
        // figures are read the same way, from their own distribution over the same windows.
        const int wanted = juce::jmax (1, (int) std::lround ((double) total_ * (double) kPercentile));
        r.loudRmsDbfs  = percentileOf (bins_, binSumDb_, wanted);
        r.loudPeakDbfs = percentileOf (peakBins_, peakSumDb_, wanted);
        r.valid = true;
        return r;
    }

    /** The wire object, or a void var under 20 s heard (the spec's null). */
    juce::var toVar() const
    {
        const auto r = read();
        if (! r.valid) return {};
        auto* o = new juce::DynamicObject();
        // TWO DECIMALS, because the convention test in section 5 is "-3.01" and one decimal cannot express it.
        o->setProperty ("loud_rms_dbfs",  juce::String (r.loudRmsDbfs, 2).getDoubleValue());
        o->setProperty ("loud_peak_dbfs", juce::String (r.loudPeakDbfs, 2).getDoubleValue());
        // THE LABEL IS THE SPEC'S, VERBATIM: "400ms_p95" (COMP_PROFILE_SPEC_v1 section 5, v1.4 - and v1.3
        // before it). This read "400ms_rms_p95", which was the v1 label and described only the RMS figure. It
        // stopped being true at v1.3, when loud_peak_dbfs was defined over THE SAME windows: the window is a
        // 400 ms p95 window shared by both figures, and naming it after one of them was wrong as well as
        // non-conforming. Found by feeding the server's own generated request through this guard - B's side
        // already sends the spec's string, so the two disagreed and only the plugin was out.
        o->setProperty ("window", "400ms_p95");
        o->setProperty ("heard_s", (int) std::lround (r.heardSeconds));
        return juce::var (o);
    }

private:
    void closeWindow() noexcept
    {
        const double meanSq = sumSq_ / (double) juce::jmax (1, windowSamples_);
        const float rms = (float) std::sqrt (meanSq);
        const float db = rms > 0.0f ? juce::Decibels::gainToDecibels (rms, kLoDb) : kLoDb;
        if (db < kSilenceGateDb) return;        // not programme: it neither counts as heard nor enters the stats
        heardSamples_ += (juce::int64) windowSamples_;
        const int bin = binForDb (db);
        ++bins_[(size_t) bin];
        // ...AND THE MEAN dB OF THE WINDOWS IN THAT BIN. The bin's own lower edge is only accurate to 0.25 dB, and
        // COMP_PROFILE_SPEC_v1 section 5 (v1.2) pins the convention by a test - "a full-scale 997 Hz sine must read
        // -3.01" - which a quarter-dB bin edge would answer as -3.00. Keeping the sum costs one float per bin and
        // makes the reported figure the real level rather than the bin it fell in.
        binSumDb_[(size_t) bin] += (double) db;
        ++total_;
        // loud_peak_dbfs, DEFINED (spec v1.3 section 5): "over the same 400 ms windows, the maximum absolute
        // sample value in each window (no oversampling), then the 95th percentile of those across what was heard."
        // So it is its OWN percentile over its OWN distribution - one max-|sample| figure per window, the same
        // windows the RMS percentile uses - and not, as v1.2 left it to the reader, the largest peak among the loud
        // windows. The two answers differ: a percentile discards the top 5% of windows, so a single transient in one
        // window cannot set it, which is the point of defining it this way.
        const float pkDb = windowPeak_ > 0.0f ? juce::Decibels::gainToDecibels (windowPeak_, kLoDb) : kLoDb;
        const int pkBin = binForDb (pkDb);
        ++peakBins_[(size_t) pkBin];
        peakSumDb_[(size_t) pkBin] += (double) pkDb;
    }

    /** The dB at the `wanted`-th window counting up from the quiet end, reported as the MEAN of the values that
        fell in that bin rather than the bin's 0.25 dB edge - section 5's convention test is to two decimals. */
    static float percentileOf (const std::array<int, (size_t) kBins>& bins,
                               const std::array<double, (size_t) kBins>& sums, int wanted) noexcept
    {
        int seen = 0, bin = kBins - 1;
        for (int b = 0; b < kBins; ++b)
        {
            seen += bins[(size_t) b];
            if (seen >= wanted) { bin = b; break; }
        }
        return bins[(size_t) bin] > 0 ? (float) (sums[(size_t) bin] / (double) bins[(size_t) bin])
                                      : dbForBin (bin);
    }

    static int   binForDb (float db) noexcept
    { return juce::jlimit (0, kBins - 1, (int) std::lround ((db - kLoDb) / kBinDb)); }
    static float dbForBin (int bin) noexcept
    { return kLoDb + (float) bin * kBinDb; }

    double sr_ = 48000.0;
    int    windowSamples_ = 19200;
    std::array<int, (size_t) kBins> bins_ {};
    std::array<double, (size_t) kBins> binSumDb_ {};   // the sum of the dB values in each bin: see closeWindow
    int    total_ = 0;
    double sumSq_ = 0.0;
    int    inWindow_ = 0;
    float  windowPeak_ = 0.0f;
    juce::int64 heardSamples_ = 0;
    juce::int64 sinceHeardGrew_ = 0;   // SAMPLES since a window last counted as heard: the "played through" clock
    // loud_peak_dbfs (v1.3): its own distribution, one max-|sample| per window, over the same windows.
    std::array<int, (size_t) kBins> peakBins_ {};
    std::array<double, (size_t) kBins> peakSumDb_ {};
};

} // namespace echojay
