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
        static constexpr double realizationDepthDb = 6.2038;   // decorrelated case only
        static double centreOct (double hz) { return hz <= 250.0 ? 0.0322 : 0.0138; }
    };

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
