/*
  EjmapProbe.h

  M9 audio probe: the render harness, stimulus, spectra, features, the
  measured noise floor, write-with-pump-confirm, and the probe stake.
  Signed for build 2026-08-02 (proposal revision 3, carve-outs folded).

  Scope of this first build: harness + preconditions (sanity, sigma_f,
  excitation-verified-by-signal) + the eq suite's features — exactly what the
  AMEK headline gate exercises. Suites 2-7 stop-and-report per the gate order.

  Measured facts this file is built on (Task 0 / 0-B of the proposal):
  - a parameter write lands only when the MESSAGE LOOP runs; the rule is
    pump-until-getValue-confirms, bounded, ms recorded (writeConfirm below);
  - once landed, propagation to the render is block 0 -- no settle constant;
  - declared latency lies in both directions; alignment never trusts it;
  - AMEK's sample-domain dither is ~3.9e-5, so every verdict threshold is a
    multiple of the measured per-feature floor, never an epsilon.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <cmath>

namespace ejmap
{

struct Probe
{
    static constexpr double kSampleRate = 48000.0;
    static constexpr int    kBlock      = 512;
    static constexpr int    kFftOrder   = 13;              // 8192 -> 4096 bins
    static constexpr int    kFftSize    = 1 << kFftOrder;
    static constexpr int    kBins       = kFftSize / 2;

    //==========================================================================
    /** Independent fixed-seed pink noise per channel (Paul Kellet filter over
        seeded white). Same seeds every capture, so captures are sample-aligned
        and deterministic plugins null exactly.
    */
    struct PinkChannel
    {
        juce::Random rng;
        float b0 = 0, b1 = 0, b2 = 0;
        explicit PinkChannel (juce::int64 seed) : rng (seed) {}
        float next()
        {
            const float w = rng.nextFloat() * 2.0f - 1.0f;
            b0 = 0.99765f * b0 + w * 0.0990460f;
            b1 = 0.96300f * b1 + w * 0.2965164f;
            b2 = 0.57000f * b2 + w * 1.0526913f;
            return 0.18f * (b0 + b1 + b2 + w * 0.1848f);
        }
    };

    /** One rendered capture: 0.5 s silence pre-roll, then 2 s pink stimulus;
        the first 250 ms after onset are discarded. Returns the analysed
        1.75 s as stereo. The CALLER owns pump pausing (per render) and the
        process lock; this function only fills, calls processBlock, collects.
    */
    static juce::AudioBuffer<float> renderCapture (juce::AudioPluginInstance& p)
    {
        const int chans = juce::jmax (2, p.getTotalNumInputChannels(),
                                         p.getTotalNumOutputChannels());
        juce::AudioBuffer<float> io (chans, kBlock);
        juce::MidiBuffer midi;

        PinkChannel left (0xE501), right (0xE502);   // independent seeds

        const int prerollBlocks = (int) (0.5 * kSampleRate) / kBlock;
        for (int k = 0; k < prerollBlocks; ++k)
        { io.clear(); midi.clear(); p.processBlock (io, midi); }

        const int stimBlocks    = (int) (2.0 * kSampleRate) / kBlock;   // 187
        const int discardBlocks = (int) (0.25 * kSampleRate) / kBlock;  // 23
        juce::AudioBuffer<float> out (2, (stimBlocks - discardBlocks) * kBlock);
        for (int k = 0; k < stimBlocks; ++k)
        {
            for (int n = 0; n < kBlock; ++n)
            {
                const float l = 0.25f * left.next(), r = 0.25f * right.next();
                io.setSample (0, n, l);
                if (chans > 1) io.setSample (1, n, r);
                for (int ch = 2; ch < chans; ++ch) io.setSample (ch, n, 0.0f);
            }
            midi.clear();
            p.processBlock (io, midi);
            if (k >= discardBlocks)
                for (int ch = 0; ch < 2; ++ch)
                    out.copyFrom (ch, (k - discardBlocks) * kBlock, io,
                                  juce::jmin (ch, chans - 1), 0, kBlock);
        }
        return out;
    }

    /** The raw stimulus alone (no plugin), for the decorrelation check and
        the sanity gate's input reference. Identical generator, identical
        seeds, identical discard.
    */
    static juce::AudioBuffer<float> stimulusReference (juce::int64 seedOffset = 0)
    {
        PinkChannel left (0xE501 + seedOffset), right (0xE502 + seedOffset);
        const int stimBlocks    = (int) (2.0 * kSampleRate) / kBlock;
        const int discardBlocks = (int) (0.25 * kSampleRate) / kBlock;
        juce::AudioBuffer<float> out (2, (stimBlocks - discardBlocks) * kBlock);
        for (int k = 0; k < stimBlocks; ++k)
            for (int n = 0; n < kBlock; ++n)
            {
                const float l = 0.25f * left.next(), r = 0.25f * right.next();
                if (k >= discardBlocks)
                {
                    out.setSample (0, (k - discardBlocks) * kBlock + n, l);
                    out.setSample (1, (k - discardBlocks) * kBlock + n, r);
                }
            }
        return out;
    }

    /** A KNOWN-TRUTH lobe: the stimulus through a textbook peaking biquad at
        exactly centreHz. Used to measure the FEATURE EXTRACTOR's own
        resolution, with no plugin and no XPC anywhere in the path -- the
        only way to separate instrument error from plugin behaviour.
    */
    static juce::AudioBuffer<float> syntheticPeak (const juce::AudioBuffer<float>& in,
                                                   double centreHz, double gainDb, double q)
    {
        juce::AudioBuffer<float> out (in);
        auto coeffs = juce::dsp::IIR::Coefficients<float>::makePeakFilter (
                          kSampleRate, (float) centreHz, (float) q,
                          juce::Decibels::decibelsToGain ((float) gainDb));
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
        {
            juce::dsp::IIR::Filter<float> f (coeffs);
            f.reset();
            auto* d = out.getWritePointer (ch);
            for (int i = 0; i < out.getNumSamples(); ++i) d[i] = f.processSample (d[i]);
        }
        return out;
    }

    //==========================================================================
    /** Welch power spectra of mid and side, 8192-point Hann, 50% overlap.
        Mid/side are formed per sample BEFORE the FFT; no summed or left-only
        spectrum exists anywhere in this header.
    */
    struct Spectra
    {
        std::vector<double> mid, side;      // kBins power values
        static double binHz (int b) { return b * kSampleRate / kFftSize; }
    };

    static Spectra welch (const juce::AudioBuffer<float>& stereo)
    {
        Spectra s; s.mid.assign (kBins, 0.0); s.side.assign (kBins, 0.0);
        juce::dsp::FFT fft (kFftOrder);
        std::vector<float> window (kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            window[i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / (kFftSize - 1));

        std::vector<float> buf (2 * kFftSize);
        int segments = 0;
        for (int start = 0; start + kFftSize <= stereo.getNumSamples(); start += kFftSize / 2)
        {
            for (int pass = 0; pass < 2; ++pass)     // 0 = mid, 1 = side
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < kFftSize; ++i)
                {
                    const float l = stereo.getSample (0, start + i);
                    const float r = stereo.getSample (1, start + i);
                    buf[(size_t) i] = window[(size_t) i]
                                        * (pass == 0 ? 0.5f * (l + r) : 0.5f * (l - r));
                }
                fft.performRealOnlyForwardTransform (buf.data());
                auto& dst = pass == 0 ? s.mid : s.side;
                for (int b = 0; b < kBins; ++b)
                {
                    const float re = buf[(size_t) (2 * b)], im = buf[(size_t) (2 * b + 1)];
                    dst[(size_t) b] += (double) re * re + (double) im * im;
                }
            }
            ++segments;
        }
        if (segments > 0)
            for (int b = 0; b < kBins; ++b)
            { s.mid[(size_t) b] /= segments; s.side[(size_t) b] /= segments; }
        return s;
    }

    static double bandEnergyDb (const std::vector<double>& power, double loHz, double hiHz)
    {
        double e = 0; int n = 0;
        for (int b = 1; b < kBins; ++b)
        {
            const double f = Spectra::binHz (b);
            if (f >= loHz && f <= hiHz) { e += power[(size_t) b]; ++n; }
        }
        return n > 0 ? 10.0 * std::log10 (e / n + 1.0e-30) : -300.0;
    }

    /** Broadband side/mid ratio of a buffer, dB. Independent equal-power
        channels measure ~0 dB; a mono-ish stimulus goes deeply negative.
    */
    static double sideMidRatioDb (const juce::AudioBuffer<float>& stereo)
    {
        double em = 0, es = 0;
        for (int i = 0; i < stereo.getNumSamples(); ++i)
        {
            const double m = 0.5 * (stereo.getSample (0, i) + stereo.getSample (1, i));
            const double sd = 0.5 * (stereo.getSample (0, i) - stereo.getSample (1, i));
            em += m * m; es += sd * sd;
        }
        return 10.0 * std::log10 ((es + 1e-30) / (em + 1e-30));
    }

    //==========================================================================
    /** The eq suite's features, from a mid difference spectrum
        dH(f) = 10 log10 (Pb / Pa).
    */
    struct LobeFeatures
    {
        double centreHz = 0, depthDb = 0, widthOct = 0;
        double maxAbsDb = 0;                 // in-band |dH| excursion
        bool   lobeFound = false;            // a coherent lobe cleared depthFloorDb

        /** A centre extracted from a lobe that does not exist is a number
            with no referent -- in the M9 gate's arm A a 0.235 dB broadband
            ripple still reported a centre, and had that ripple peaked near
            the predicted frequency the verdict would have been WRONG for the
            right-looking reason. Callers must ask this before reading
            centreHz, and print "undefined" when it is false.
        */
        bool centreDefined() const { return lobeFound; }
    };

    /** depthFloorDb: a lobe must clear this to have a centre at all. The
        caller passes the derived floor (0.25 * expressed Delta_pred, or the
        feature's sigma floor, whichever is larger) -- never a taste constant
        chosen here.
    */
    static LobeFeatures lobeFeatures (const std::vector<double>& pa,
                                      const std::vector<double>& pb,
                                      double loHz = 30.0, double hiHz = 20000.0,
                                      double depthFloorDb = 0.0)
    {
        LobeFeatures f;
        int peak = -1; double peakAbs = 0;
        std::vector<double> dh (kBins, 0.0);
        for (int b = 1; b < kBins; ++b)
        {
            dh[(size_t) b] = 10.0 * std::log10 ((pb[(size_t) b] + 1e-30) / (pa[(size_t) b] + 1e-30));
            const double fhz = Spectra::binHz (b);
            if (fhz < loHz || fhz > hiHz) continue;
            const double a = std::abs (dh[(size_t) b]);
            f.maxAbsDb = juce::jmax (f.maxAbsDb, a);
            if (a > peakAbs) { peakAbs = a; peak = b; }
        }
        if (peak < 1) return f;
        f.depthDb = dh[(size_t) peak];
        f.lobeFound = std::abs (f.depthDb) >= depthFloorDb;   // no lobe -> no centre
        // parabolic refinement in dB around the peak bin
        double centreBin = peak;
        if (peak > 1 && peak < kBins - 1)
        {
            const double y0 = std::abs (dh[(size_t) (peak - 1)]),
                         y1 = std::abs (dh[(size_t) peak]),
                         y2 = std::abs (dh[(size_t) (peak + 1)]);
            const double den = y0 - 2 * y1 + y2;
            if (std::abs (den) > 1e-12) centreBin = peak + 0.5 * (y0 - y2) / den;
        }
        f.centreHz = centreBin * kSampleRate / kFftSize;
        // -3 dB width around the peak
        const double edge = std::abs (f.depthDb) - 3.0;
        int lo = peak, hi = peak;
        while (lo > 1 && std::abs (dh[(size_t) (lo - 1)]) >= edge) --lo;
        while (hi < kBins - 1 && std::abs (dh[(size_t) (hi + 1)]) >= edge) ++hi;
        const double fLo = Spectra::binHz (lo), fHi = Spectra::binHz (hi);
        f.widthOct = fLo > 0 ? std::log2 (fHi / fLo) : 0.0;
        return f;
    }

    //==========================================================================
    // COMPRESSOR / LIMITER / GATE stimuli and features. The stepped tone is
    // the static I/O curve; the burst train is the envelope. Both are
    // deterministic and seed-free (a tone, not noise), so A/B pairs null
    // exactly on a deterministic plugin.
    //==========================================================================

    /** -70 .. 0 dBFS in 2 dB steps. The first build used -40..0, which on
        API-2500 at max ratio left NO unity-slope region inside the stimulus
        -- the two-segment fit then had nothing to anchor its lower segment
        to and put the knee at -37 dB for a threshold set at -20. Measured
        on synthetic truth the fit is exact; measured on the plugin it was
        worse than the grid it replaced. The stimulus, not the estimator,
        was the defect: a knee can only be found inside the range you show
        the plugin. 36 steps costs 10.8 s of rendered audio per capture,
        ~50 ms wall on a bridged subject.
    */
    static constexpr int    kSteps       = 46;      // -90 .. 0 dBFS peak, 2 dB apart
    static constexpr double kStepBaseDb  = -90.0;   // low enough to see a gate
                                                // threshold's BOTH sides (X-Gate
                                                // ladder reaches -72)
    static constexpr double kStepSeconds = 0.30;

    /** Envelope RMS window in SAMPLES. 8 samples = 0.167 ms at 48 kHz.
        Measured instrument floor for tau extraction (known-truth exponentials,
        --gate-m9 taufloor): see InstrumentFloor::tauMs. A tau below that floor
        is UNDEFINED and must be reported as such, never fitted -- the first
        run of this suite reported 4 ms for a true 0.03 ms attack, which was
        the window's own smoothing, not the plugin.
    */
    static constexpr int kEnvWindow = 8;

    /** A known-truth exponential envelope: starts at fromDb, approaches toDb
        with time constant tauMs, sampled at kEnvWindow resolution. Used to
        measure what the extractor can actually resolve, with no plugin.
    */
    static juce::Array<double> syntheticEnvelope (double fromDb, double toDb,
                                                  double tauMs, int lengthMs)
    {
        juce::Array<double> env;
        const double msPerSample = 1000.0 * kEnvWindow / kSampleRate;
        const int n = (int) (lengthMs / msPerSample);
        for (int i = 0; i < n; ++i)
        {
            const double t = i * msPerSample;
            env.add (toDb + (fromDb - toDb) * std::exp (-t / juce::jmax (1e-6, tauMs)));
        }
        return env;
    }

    /** Renders the stepped 997 Hz tone and returns the measured OUTPUT level
        of each step in dBFS, read over the LAST 40% of the step so the
        attack transient has passed. Index i is input level -40 + 2i dBFS.
    */
    static juce::Array<double> steppedCurve (juce::AudioPluginInstance& p)
    {
        const int chans = juce::jmax (2, p.getTotalNumInputChannels(),
                                         p.getTotalNumOutputChannels());
        juce::AudioBuffer<float> io (chans, kBlock);
        juce::MidiBuffer midi;
        double phase = 0.0;
        const double step = juce::MathConstants<double>::twoPi * 997.0 / kSampleRate;

        for (int k = 0; k < (int) (0.5 * kSampleRate) / kBlock; ++k)
        { io.clear(); midi.clear(); p.processBlock (io, midi); }

        const int blocksPerStep = (int) (kStepSeconds * kSampleRate) / kBlock;
        const int readFrom = (int) (blocksPerStep * 0.6);
        juce::Array<double> out;
        for (int sIdx = 0; sIdx < kSteps; ++sIdx)
        {
            const double amp = std::pow (10.0, (kStepBaseDb + 2.0 * sIdx) / 20.0);
            double acc = 0; int accN = 0;
            for (int k = 0; k < blocksPerStep; ++k)
            {
                for (int n = 0; n < kBlock; ++n)
                {
                    const float v = (float) (amp * std::sin (phase));
                    for (int ch = 0; ch < chans; ++ch) io.setSample (ch, n, v);
                    phase += step;
                    if (phase > juce::MathConstants<double>::twoPi)
                        phase -= juce::MathConstants<double>::twoPi;
                }
                midi.clear();
                p.processBlock (io, midi);
                if (k >= readFrom)
                {
                    for (int n = 0; n < kBlock; ++n)
                    { const double v = io.getSample (0, n); acc += v * v; }
                    accN += kBlock;
                }
            }
            // LEVEL CONVENTION, declared (audit item 1): the stimulus step is
            // a PEAK amplitude (amp = 10^(dB/20) of a sine), so the output
            // must be reported in the SAME convention or the curve is offset
            // by 20*log10(sqrt(2)) = 3.01 dB and every unity assumption in
            // the estimators is wrong by that much. RMS of a sine is peak
            // minus 3.01 dB, so the conversion is exact for this stimulus and
            // is applied here rather than left for each caller to remember.
            // This is the same 3.01 dB the limiter plateau exposed; it was
            // never limiter-only.
            const double rmsDb = accN > 0 ? 20.0 * std::log10 (std::sqrt (acc / accN) + 1e-12) : -200.0;
            out.add (rmsDb + 3.0103);      // -> PEAK dBFS, matching the input axis
        }
        return out;
    }

    struct CurveFeatures
    {
        double kneeInDb = 0;        // input level where the curve leaves 1:1
        double slopeAbove = 1.0;    // dOut/dIn above the knee (1/ratio)
        double offsetDb = 0;        // output at the loudest step
        bool   kneeFound = false;
    };

    /** Knee = the highest input level at which the local slope is still ~1,
        walking DOWN from the top; slope above = least-squares fit over the
        steps above it. A curve that never departs 1:1 reports kneeFound
        false, and the caller must not invent a threshold from it -- the
        same discipline as an undefined lobe centre.
    */
    static CurveFeatures curveFeatures (const juce::Array<double>& outDb,
                                        double slopeFloor = 0.9)
    {
        CurveFeatures f;
        if (outDb.size() < 6) return f;
        juce::Array<double> slope;                       // local dOut/dIn
        for (int i = 1; i < outDb.size(); ++i) slope.add ((outDb[i] - outDb[i - 1]) / 2.0);
        f.offsetDb = outDb.getLast();

        int kneeIdx = -1;
        for (int i = slope.size() - 1; i >= 1; --i)
            if (slope[i] >= slopeFloor && slope[i - 1] >= slopeFloor) { kneeIdx = i; break; }
        if (kneeIdx < 0 || kneeIdx >= slope.size() - 1) return f;      // never compresses, or always
        f.kneeFound = true;
        f.kneeInDb = kStepBaseDb + 2.0 * (kneeIdx + 1);

        double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
        for (int i = kneeIdx + 1; i < outDb.size(); ++i)
        {
            const double x = kStepBaseDb + 2.0 * i, y = outDb[i];
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
        }
        if (n >= 2)
        {
            const double den = n * sxx - sx * sx;
            if (std::abs (den) > 1e-9) f.slopeAbove = (n * sxy - sx * sy) / den;
        }
        return f;
    }

    /** See declaration above. Breakpoint search on a 0.1 dB grid across the
        stimulus range; below it the curve is forced to unity slope (which is
        what a compressor below threshold does), above it a free least-squares
        line. The reported knee is the breakpoint, NOT a step index, so its
        resolution is no longer the stimulus's 2 dB grid.
    */
    static CurveFeatures curveFeaturesTwoSegment (const juce::Array<double>& outDb)
    {
        CurveFeatures f;
        if (outDb.size() < 6) return f;
        f.offsetDb = outDb.getLast();
        // CONTINUITY-CONSTRAINED hard-knee model: below the breakpoint the
        // curve is y = x (a compressor passes unity below threshold); above
        // it y = knee + (x - knee) * slope, meeting the lower segment exactly
        // AT the knee. The first implementation left the two segments
        // independent, which let the fit slide a whole step low -- measured
        // 2.00 dB worst error against the step grid's 1.60, i.e. worse than
        // the grid it was meant to beat. One free parameter per breakpoint.
        double bestErr = 1e30, bestKnee = 0, bestSlope = 1.0;
        for (double knee = kStepBaseDb + 2.0; knee <= -2.0; knee += 0.1)
        {
            double num = 0, den = 0; int n = 0;
            for (int i = 0; i < outDb.size(); ++i)
            {
                const double x = kStepBaseDb + 2.0 * i;
                if (x <= knee) continue;
                const double dx = x - knee;
                num += dx * (outDb[i] - knee); den += dx * dx; ++n;
            }
            if (n < 3 || den < 1e-9) continue;
            const double m = num / den;
            double err = 0;
            for (int i = 0; i < outDb.size(); ++i)
            {
                const double x = kStepBaseDb + 2.0 * i;
                const double pred = x <= knee ? x : knee + (x - knee) * m;
                err += (outDb[i] - pred) * (outDb[i] - pred);
            }
            if (err < bestErr) { bestErr = err; bestKnee = knee; bestSlope = m; }
        }
        f.kneeFound = bestErr < 1e29 && bestSlope < 0.95;
        f.kneeInDb = bestKnee;
        f.slopeAbove = bestSlope;
        return f;
    }

    /** GATE feature (bottom-of-curve): the input level at which output
        DEPARTS UNITY GOING DOWN, walked from the loudest step downward. The
        top-of-curve plateau estimator is structurally wrong for this
        category -- a gate is open at the top whatever its threshold, which
        is why the plateau read a constant -7.01 dB across every setting.

        Both axes are dBFS PEAK after the convention audit, so the unity
        region is departure ~= 0 and needs no offset. Returns the highest
        input level whose departure first reaches departureDb, or -1000 when
        the curve never departs (a gate that never closes inside the
        stimulus) -- stated as undefined, never fitted.
    */
    static double gateOpenPointDb (const juce::Array<double>& outDb,
                                   double departureDb = 3.0)
    {
        for (int i = outDb.size() - 1; i >= 0; --i)
        {
            const double in = kStepBaseDb + 2.0 * i;
            if (in - outDb[i] >= departureDb)
                return in;
        }
        return -1000.0;
    }

    /** Gain reduction at the loudest step: input minus output, both peak.
        Near zero means the stimulus never drove the processor, so there was
        no plateau to read -- the distinction the limiter's 4.82 dB point
        needs before anything is built around it.
    */
    static double grAtTopDb (const juce::Array<double>& outDb)
    {
        if (outDb.isEmpty()) return 0.0;
        const double in = kStepBaseDb + 2.0 * (outDb.size() - 1);
        return in - outDb.getLast();
    }

    /** Burst train over a QUIET BED: 400 ms at -6 dBFS alternating with
        600 ms at -30 dBFS (NOT silence), 997 Hz, 4 cycles. Returns the output
        envelope in dB at kEnvWindow resolution.

        The bed is the correction for a defect this stimulus had when first
        run: with silence between bursts, gain RECOVERY is invisible -- the
        output is silent whatever the gain is doing, so release measured
        UNDEFINED by construction on API-2500. A quiet bed makes the release
        trajectory observable without re-triggering compression.
    */
    static juce::Array<double> burstEnvelope (juce::AudioPluginInstance& p,
                                             bool dense = false)
    {
        const int chans = juce::jmax (2, p.getTotalNumInputChannels(),
                                         p.getTotalNumOutputChannels());
        juce::AudioBuffer<float> io (chans, kBlock);
        juce::MidiBuffer midi;
        double phase = 0.0;
        const double step = juce::MathConstants<double>::twoPi * 997.0 / kSampleRate;
        const double amp = std::pow (10.0, -6.0 / 20.0);

        for (int k = 0; k < (int) (0.5 * kSampleRate) / kBlock; ++k)
        { io.clear(); midi.clear(); p.processBlock (io, midi); }

        const int totalBlocks = (int) (4.0 * kSampleRate) / kBlock;
        const int onSamples = (int) ((dense ? 0.12 : 0.4) * kSampleRate),
                  periodSamples = (int) ((dense ? 0.30 : 1.0) * kSampleRate);
        const double bedAmp = std::pow (10.0, -30.0 / 20.0);
        juce::Array<double> env;
        long nAbs = 0;
        const int win = kEnvWindow;
        double acc = 0; int accN = 0;
        for (int k = 0; k < totalBlocks; ++k)
        {
            for (int n = 0; n < kBlock; ++n)
            {
                const bool on = (nAbs % periodSamples) < onSamples;
                const float v = (float) ((on ? amp : bedAmp) * std::sin (phase));
                for (int ch = 0; ch < chans; ++ch) io.setSample (ch, n, v);
                phase += step;
                if (phase > juce::MathConstants<double>::twoPi)
                    phase -= juce::MathConstants<double>::twoPi;
                ++nAbs;
            }
            midi.clear();
            p.processBlock (io, midi);
            for (int n = 0; n < kBlock; ++n)
            {
                const double v = io.getSample (0, n);
                acc += v * v; ++accN;
                if (accN >= win)
                { env.add (20.0 * std::log10 (std::sqrt (acc / accN) + 1e-12)); acc = 0; accN = 0; }
            }
        }
        return env;
    }

    /** Time constant from an envelope segment: ms to cover 63.2% of the
        total excursion between startMs and endMs. Returns -1 when the
        excursion never clears minExcursionDb -- an undefined tau, stated,
        never a fitted number over noise.
    */
    static double envMsPerSample() { return 1000.0 * kEnvWindow / kSampleRate; }

    /** startMs/endMs are MILLISECONDS into the envelope; the conversion to
        envelope samples happens here so callers never carry the resolution.
    */
    /** ITEM 2: a tau alone cannot be read. The excursion the estimator had
        available and the residual of the exponential fit travel WITH it, so
        a constant-looking number can be told apart from a real plateau: a
        sub-dB excursion means the estimator had nothing to fit, and the
        reason recorded on disk must be "unresolvable on this material",
        never "program dependence".
    */
    struct TauResult
    {
        double tauMs = -1, excursionDb = 0, residualDb = -1, windowMs = 0;
        bool   resolvable() const
        { return tauMs > 0 && excursionDb >= 1.0 && tauMs < 0.95 * windowMs; }

        /** The recorded reason must be the TRUE one. Three distinct failures
            hide behind a constant-looking tau, and the API-2500 dense-material
            case walked through two of them before landing on the third:
              - excursion below 1 dB   -> nothing to fit
              - tau at the window edge -> the window is shorter than the
                                          recovery; the number is the window,
                                          not the plugin (measured: 179.0 ms
                                          five times, which IS the 179 ms
                                          window, with 35-45 dB of excursion
                                          available and a 6-10 dB fit residual)
              - high residual          -> the envelope is not exponential here
        */
        juce::String why() const
        {
            if (excursionDb < 1.0)
                return "unresolvable on this material: gain-reduction excursion "
                     + juce::String (excursionDb, 2) + " dB, below the 1 dB the fit needs";
            if (tauMs >= 0.95 * windowMs)
                return "WINDOW-LIMITED, not resolved: tau " + juce::String (tauMs, 1)
                     + " ms is the measurement window itself (" + juce::String (windowMs, 1)
                     + " ms), so recovery never completed inside it -- the material's "
                       "off-period is shorter than this plugin's release";
            if (residualDb > 5.0)
                return "poor exponential fit (residual " + juce::String (residualDb, 2)
                     + " dB): the envelope is not a single exponential on this material";
            return "resolvable";
        }
    };

    static TauResult timeConstantFull (const juce::Array<double>& env,
                                       double startMs, double endMs)
    {
        TauResult r;
        r.windowMs = endMs - startMs;
        const double mps = envMsPerSample();
        const int s = (int) (startMs / mps), e = (int) (endMs / mps);
        if (e >= env.size() || s >= e) return r;
        const double a = env[s], b = env[e];
        r.excursionDb = std::abs (b - a);
        if (r.excursionDb < 1.0) return r;
        const double target = a + 0.632 * (b - a);
        for (int i = s; i <= e; ++i)
            if ((b > a && env[i] >= target) || (b < a && env[i] <= target))
            { r.tauMs = (i - s) * mps; break; }
        if (r.tauMs > 0)
        {
            double sq = 0; int n = 0;
            for (int i = s; i <= e; ++i)
            {
                const double t = (i - s) * mps;
                const double pred = b + (a - b) * std::exp (-t / r.tauMs);
                sq += (env[i] - pred) * (env[i] - pred); ++n;
            }
            r.residualDb = n > 0 ? std::sqrt (sq / n) : -1;
        }
        return r;
    }

    static double timeConstantMs (const juce::Array<double>& env,
                                  double startMs, double endMs, double minExcursionDb)
    {
        const double mps = envMsPerSample();
        const int s = (int) (startMs / mps), e = (int) (endMs / mps);
        if (e >= env.size() || s >= e) return -1.0;
        const double a = env[s], b = env[e];
        if (std::abs (b - a) < minExcursionDb) return -1.0;
        const double target = a + 0.632 * (b - a);
        for (int i = s; i <= e; ++i)
            if ((b > a && env[i] >= target) || (b < a && env[i] <= target))
                return (i - s) * mps;
        return -1.0;
    }

    //==========================================================================
    /** MEASURED INSTRUMENT FLOORS (gate amendment 3, measured 2026-08-02 by
        --gate-m9 instrument, no plugin in the path). sigma_f from a plugin
        A/A pair captures REPEAT noise only, and a deterministic plugin has
        none -- AMEK measured sigma_side = 0.000, which made 4*sigma a no-op.
        These are the extractor's own resolution, and every sigma_f is
        floored at them.

          numerical (bit-identical pair)          0.000000 dB  -- exactly
                                                     deterministic, no
                                                     spurious noise added
          known-truth depth error (+6.00 dB peak) 0.088 dB
          known-truth centre error, <= 250 Hz     0.0322 oct
          known-truth centre error, >  250 Hz     0.0138 oct
          realization width spread                0.0265 oct
          realization side-band spread            0.4857 dB

        The realization numbers come from independent noise realizations
        through an unchanged system. The harness uses FIXED seeds, so A and B
        share their realization exactly and this variance cancels -- which is
        why they are floors, not budgets. The 6.2 dB max-bin realization
        spread is deliberately NOT used as a depth floor: it is the figure
        that would apply if a time-variant plugin decorrelated the
        realization, and it is recorded for that case alone.
    */
    struct InstrumentFloor
    {
        static constexpr double depthDb   = 0.088;
        static constexpr double widthOct  = 0.0265;
        static constexpr double sideDb    = 0.4857;
        /** tau floor: the shortest time constant the envelope extractor can
            resolve, measured against known-truth exponentials. Below it a tau
            is UNDEFINED, never fitted. Measured by --gate-m9 taufloor. */
        static constexpr double tauMs     = 0.5;
        static constexpr double realizationDepthDb = 6.2038;   // decorrelated case only
        static double centreOct (double hz) { return hz <= 250.0 ? 0.0322 : 0.0138; }

        /** The MEASURED per-frequency centre bias (signed error extracting a
            known-truth peak, --gate-m9 instrument). Item 1 of the review:
            the bias is systematic, so it enters the confirm tolerance as an
            ADDITIVE bias term -- never as 4*sigma, and never omitted. At
            160 Hz it is 0.0322 oct, 46% of the signed 0.070 gate, which is
            why omitting it was not acceptable either.
        */
        static double centreBiasOct (double hz)
        {
            static const double f[] = { 40, 63, 80, 100, 160, 250, 400, 630,
                                        1000, 2000, 4000, 8000, 16000 };
            static const double b[] = { 0.0158, 0.0152, 0.0028, 0.0051, 0.0322, 0.0077,
                                        0.0138, 0.0071, 0.0022, 0.0042, 0.0032, 0.0035, 0.0007 };
            const int n = 13;
            if (hz <= f[0]) return b[0];
            if (hz >= f[n - 1]) return b[n - 1];
            for (int i = 1; i < n; ++i)
                if (hz <= f[i])
                {
                    const double t = std::log2 (hz / f[i - 1]) / std::log2 (f[i] / f[i - 1]);
                    return b[i - 1] + t * (b[i] - b[i - 1]);
                }
            return b[n - 1];
        }
    };

    //==========================================================================
    /** THE MODE-TOKEN GUARD, harness-level (item 4). A parameter whose display
        does not parse as a leading number is in a MODE where its numeric
        semantics do not apply, and no numeric verdict may be issued against
        it. This is not compressor-specific and must not be re-implemented per
        suite -- API-2500's Release displays "Var" and the compressor suite
        issued verdicts against it until this guard existed.

        THERE IS NO TOKEN ALLOWLIST, deliberately. The test is numeric
        parseability through the shared parseLeadingFloat, so it FAILS SAFE by
        construction: any token the project has never seen -- a vendor's
        "Prog", "Adapt", a localised string, a glyph -- is non-numeric,
        therefore fires the guard, therefore yields inconclusive. An allowlist
        would fail OPEN on exactly the tokens nobody anticipated, which is the
        silent-drop class. The vocabulary below is used ONLY to name the
        likely mode in the human-readable reason; an unrecognised token is
        still guarded and is quoted verbatim so the human sees what the
        plugin actually said.
    */
    static bool displayIsModeToken (const juce::String& display)
    {
        double dummy = 0;
        return ! echojay::parseLeadingFloat (display, dummy);
    }

    static juce::String modeTokenReason (const juce::String& display)
    {
        const auto d = display.trim().toLowerCase();
        auto named = [&d] (const char* t) { return d.startsWith (t); };
        juce::String likely;
        if (named ("var"))                       likely = "program-dependent (variable) mode";
        else if (named ("auto"))                 likely = "automatic mode";
        else if (named ("sync") || named ("tempo")) likely = "tempo-synced mode";
        else if (named ("ext") || named ("side"))   likely = "external/sidechain source";
        else if (named ("link"))                 likely = "linked mode";
        else if (named ("off") || named ("byp"))  likely = "disengaged";
        else                                     likely = "an unrecognised mode token (guarded anyway)";
        return "displays '" + display + "' -- " + likely
             + ", so its numeric semantics do not apply";
    }

    /** What fraction of a ladder lies above a resolution floor. A map whose
        slow half was tested must not read as verified (review, this round).
    */
    static double ladderFractionAbove (const juce::Array<juce::Array<float>>& anchors,
                                       double floorValue)
    {
        if (anchors.isEmpty()) return 0.0;
        int above = 0;
        for (const auto& a : anchors) if (a[0] >= floorValue) ++above;
        return (double) above / anchors.size();
    }

    //==========================================================================
    /** KNOWN-TRUTH static compressor: the exact output level of each stepped
        input, hard knee, for measuring what the knee and slope estimators can
        actually resolve with no plugin in the path.
    */
    static juce::Array<double> syntheticCurve (double thresholdDb, double ratio,
                                               double makeupDb = 0.0)
    {
        juce::Array<double> out;
        for (int i = 0; i < kSteps; ++i)
        {
            const double in = kStepBaseDb + 2.0 * i;
            const double o = in <= thresholdDb ? in
                                               : thresholdDb + (in - thresholdDb) / ratio;
            out.add (o + makeupDb);
        }
        return out;
    }

    /** TWO-SEGMENT FIT: recovers sub-step knee resolution from the renders
        already taken, by fitting a 1:1 line below a breakpoint and a free
        line above it, and choosing the breakpoint (on a 0.1 dB grid, off the
        step grid entirely) that minimises total squared error.
    */


    /** Write with pump-until-getValue-confirms (Task 0-B rule). Returns the
        milliseconds the landing took; -1 when the bound expired unlanded.
        MESSAGE THREAD ONLY (it pumps the dispatch loop the XPC reply needs).
    */
    static double writeConfirm (juce::AudioProcessorParameter& p, float v,
                                int boundMs = 500)
    {
        p.setValueNotifyingHost (v);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        while (std::abs (p.getValue() - v) > 0.005f)
        {
            if (juce::Time::getMillisecondCounterHiRes() - t0 > boundMs)
                return -1.0;
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        }
        return juce::Time::getMillisecondCounterHiRes() - t0;
    }

    //==========================================================================
    /** Anchor-table evaluation both ways, through the REAL consumer path.
        valueToNorm goes through interpolateAnchors; normToValue inverts the
        dominant-monotonic table by linear interpolation over its rows --
        the LADDER rule: predictions come from the table, never the request.
    */
    static double predictedLanding (const juce::Array<juce::Array<float>>& anchors,
                                    double requestedValue)
    {
        auto eff = echojay::dominantMonotonicTable (anchors);
        if (! eff.ok || eff.table.size() < 2) return requestedValue;
        const float n = echojay::interpolateAnchors (eff.table, (float) requestedValue);
        // forward-eval the table at n
        auto rows = eff.table;
        for (int i = 1; i < rows.size(); ++i)
        {
            const float n0 = rows[i - 1][1], n1 = rows[i][1];
            if ((n >= juce::jmin (n0, n1) && n <= juce::jmax (n0, n1)) || i == rows.size() - 1)
            {
                const float t = std::abs (n1 - n0) > 1e-9f ? (n - n0) / (n1 - n0) : 0.0f;
                return rows[i - 1][0] + t * (rows[i][0] - rows[i - 1][0]);
            }
        }
        return requestedValue;
    }
};

} // namespace ejmap
