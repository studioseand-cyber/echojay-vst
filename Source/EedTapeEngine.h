/*
    EedTapeEngine.h  —  the DSP behind "EchoJay Tape".

    A face on EedHarmonicCore's parts (curves, oversampler, delay line, filters)
    rather than a wrapper around HarmonicCore itself: tape's dry path has to be
    delayed by the transport delay as well as the oversampling latency, so it
    needs its own topology.

    Signal path, in the order the machine imposes it:

        in -> transport (wow + flutter, a modulated delay)
           -> tape saturation with bias, oversampled
           -> head bump (a low peak whose frequency tracks tape speed)
           -> HF loss (also speed-dependent)
           -> dry/wet, dry delayed to match
           -> output

    LATENCY. The wow/flutter delay has to be able to modulate in BOTH directions,
    so the transport sits at a fixed 2.5 ms centre delay that the modulation moves
    around. That centre is real latency and is reported (with the oversampler's,
    about 165 samples at 48 kHz), so the host compensates it and a parallel copy
    of the track does not end up 3 ms early. Reporting only the oversampler's part
    would be the same bug as not reporting any of it.

    JUCE-free, same discipline as EqEngine. Header-only: every function is short
    and lives on a per-sample loop.
*/

#pragma once

#include "EedHarmonicCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace echojay
{

class TapeEngine
{
public:
    // Advertised ranges — the SAME numbers the ParamSchema publishes.
    static constexpr float kMinSpeedIps  =  3.75f, kMaxSpeedIps  = 30.0f;
    static constexpr float kMinDriveDb   =   0.0f, kMaxDriveDb   = 24.0f;
    static constexpr float kMinBias      =-100.0f, kMaxBias      = 100.0f;
    static constexpr float kMinWow       =   0.0f, kMaxWow       = 100.0f;
    static constexpr float kMinFlutter   =   0.0f, kMaxFlutter   = 100.0f;
    static constexpr float kMinBumpDb    =   0.0f, kMaxBumpDb    =  6.0f;
    static constexpr float kMinOutDb     = -24.0f, kMaxOutDb     = 24.0f;

    // The transport's fixed centre delay, and the most the modulation may move
    // it. The centre must exceed the sum of the depths or a deep wow would read
    // past the write head into the future.
    static constexpr float kTransportMs  = 2.5f;
    static constexpr float kMaxWowMs     = 1.0f;
    static constexpr float kMaxFlutterMs = 0.2f;

    // 4x: the tape curve is soft, but bias makes it asymmetric, and asymmetry
    // makes even harmonics that fold from lower down than odd ones do.
    static constexpr int kOversampling = 4;

    TapeEngine() = default;

    void prepare (double sampleRate, int maxBlockSize)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        maxBlock_   = std::max (1, maxBlockSize);

        transportSamples_ = kTransportMs * 0.001f * (float) sampleRate_;
        osLatency_        = harmonic::Oversampler::latencyForFactor (kOversampling);
        latency_          = osLatency_ + (int) std::lround (transportSamples_);

        const int transportMax = (int) std::ceil (transportSamples_
                                                  + (kMaxWowMs + kMaxFlutterMs) * 2.0f
                                                    * 0.001f * (float) sampleRate_) + 4;

        for (auto& ch : channels_)
        {
            ch.ovs.prepare (kOversampling, maxBlock_);
            ch.transport.prepare (transportMax);
            ch.dry.prepare (latency_ + 4);
            ch.dc.prepare (sampleRate_);
            ch.bump.prepare (sampleRate_);
            ch.loss.prepare (sampleRate_);
            ch.scratch.assign ((std::size_t) maxBlock_, 0.0f);
        }

        applySpeedDependentFilters();
        reset();
    }

    void reset() noexcept
    {
        const float g = std::pow (10.0f, driveDb_.load() * 0.05f);

        for (auto& ch : channels_)
        {
            ch.ovs.reset();
            ch.transport.reset();
            ch.dry.reset();
            ch.dc.reset();
            ch.bump.reset();
            ch.loss.reset();

            ch.drive.snap (g);
            ch.comp .snap (harmonic::driveCompensation (harmonic::Curve::Tape, g));
            ch.bias .snap (biasOffset (bias_.load()));
            ch.wet  .snap (mix_.load() * 0.01f);
            ch.dryG .snap (1.0f - mix_.load() * 0.01f);
            ch.out  .snap (std::pow (10.0f, outDb_.load() * 0.05f));
        }

        wowPhase_ = 0.0;
        wowPhase2_ = 0.0;
        flutterPhase_ = 0.0;
        flutterPhase2_ = 0.0;
    }

    int latencySamples() const noexcept { return latency_; }

    // ---- parameters (message thread) --------------------------------------
    void setSpeedIps (float ips) noexcept
    {
        speedIps_.store (std::clamp (ips, kMinSpeedIps, kMaxSpeedIps));
        applySpeedDependentFilters();
    }
    void setDriveDb   (float db) noexcept { driveDb_.store (db); }
    void setBias      (float b)  noexcept { bias_.store (b); }
    void setWow       (float w)  noexcept { wow_.store (w); }
    void setFlutter   (float f)  noexcept { flutter_.store (f); }
    void setHeadBumpDb(float db) noexcept { bumpDb_.store (db); applySpeedDependentFilters(); }
    void setMixPercent(float pc) noexcept { mix_.store (pc); }
    void setOutputDb  (float db) noexcept { outDb_.store (db); }

    float getSpeedIps()   const noexcept { return speedIps_.load(); }
    float getDriveDb()    const noexcept { return driveDb_.load(); }
    float getBias()       const noexcept { return bias_.load(); }
    float getWow()        const noexcept { return wow_.load(); }
    float getFlutter()    const noexcept { return flutter_.load(); }
    float getHeadBumpDb() const noexcept { return bumpDb_.load(); }
    float getMixPercent() const noexcept { return mix_.load(); }
    float getOutputDb()   const noexcept { return outDb_.load(); }

    // The frequencies the current speed implies. Exposed so the editor can show
    // what the SPEED dial is actually doing rather than just a number.
    float headBumpHz() const noexcept { return bumpHzFor (speedIps_.load()); }
    float hfLossHz()   const noexcept { return lossHzFor (speedIps_.load(), sampleRate_); }

    // A machine at a given speed wobbles by a fixed mechanical amount, which is a
    // bigger fraction of a shorter length of tape — so the same wow reads deeper
    // at 7.5 ips than at 30.
    static float speedWobbleScale (float ips) noexcept
    {
        return std::clamp (15.0f / std::max (1.0f, ips), 0.5f, 2.0f);
    }

    static float bumpHzFor (float ips) noexcept { return std::clamp (3.3f * ips, 12.0f, 120.0f); }

    static float lossHzFor (float ips, double sampleRate) noexcept
    {
        const float nyquistish = (float) (sampleRate * 0.45);
        return std::clamp (1000.0f * ips, 2000.0f, nyquistish);
    }

    // Bias in percent -> the offset fed to the curve. Kept small: past about 0.6
    // the tape curve is so far off its linear region that bias stops being a
    // colour and starts being a fault.
    static float biasOffset (float biasPercent) noexcept
    {
        return std::clamp (biasPercent, kMinBias, kMaxBias) * 0.006f;
    }

    // ---- audio thread ------------------------------------------------------
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (numSamples <= 0 || left == nullptr) return;

        const int maxChunk = std::max (1, channels_[0].ovs.maxBlockSize());

        for (int offset = 0; offset < numSamples; offset += maxChunk)
        {
            const int n = std::min (maxChunk, numSamples - offset);
            processChunk (left + offset, right != nullptr ? right + offset : nullptr, n);
        }
    }

private:
    struct Channel
    {
        harmonic::Oversampler ovs;
        harmonic::DelayLine   transport;   // wow/flutter
        harmonic::DelayLine   dry;
        harmonic::DCBlock     dc;
        harmonic::PeakBiquad  bump;
        harmonic::OnePoleLP   loss;

        std::vector<float> scratch;

        harmonic::SmoothedValue drive, comp, bias, wet, dryG, out;
    };

    void applySpeedDependentFilters() noexcept
    {
        const float ips = speedIps_.load();
        const float hz  = bumpHzFor (ips);
        const float db  = bumpDb_.load();

        for (auto& ch : channels_)
        {
            // Q 1.0: wide enough to read as weight rather than as a resonance.
            ch.bump.setPeak (hz, 1.0f, db);
            ch.loss.setCutoff (lossHzFor (ips, sampleRate_));
        }
    }

    void processChunk (float* l, float* r, int n) noexcept
    {
        const float coeff  = harmonic::blockCoefficient (sampleRate_, n, 0.020);
        const float driveG = std::pow (10.0f, driveDb_.load() * 0.05f);
        const float wet    = std::clamp (mix_.load() * 0.01f, 0.0f, 1.0f);
        const float outG   = std::pow (10.0f, outDb_.load() * 0.05f);
        const float biasB  = biasOffset (bias_.load());

        const int numCh = r != nullptr ? 2 : 1;

        for (int c = 0; c < numCh; ++c)
        {
            auto& ch = channels_[c];
            ch.drive.startBlock (driveG, n, coeff);
            ch.comp .startBlock (harmonic::driveCompensation (harmonic::Curve::Tape, driveG), n, coeff);
            ch.bias .startBlock (biasB, n, coeff);
            ch.wet  .startBlock (wet, n, coeff);
            ch.dryG .startBlock (1.0f - wet, n, coeff);
            ch.out  .startBlock (outG, n, coeff);
        }

        // ---- transport: one wobble, both channels ---------------------------
        // The modulation is computed ONCE and applied to both channels. Two
        // independent wobbles would pull the image apart into a chorus, which is
        // a different (and much more obvious) effect than tape.
        const float ips    = speedIps_.load();
        const float scale  = speedWobbleScale (ips);
        const float wowD   = wow_.load()     * 0.01f * kMaxWowMs     * scale * 0.001f * (float) sampleRate_;
        const float flutD  = flutter_.load() * 0.01f * kMaxFlutterMs * scale * 0.001f * (float) sampleRate_;

        const double wowInc   = 2.0 * 3.14159265358979323846 * 0.6  / sampleRate_;
        const double wowInc2  = 2.0 * 3.14159265358979323846 * 0.29 / sampleRate_;
        const double flutInc  = 2.0 * 3.14159265358979323846 * 7.4  / sampleRate_;
        const double flutInc2 = 2.0 * 3.14159265358979323846 * 11.7 / sampleRate_;

        for (int i = 0; i < n; ++i)
        {
            // Two incommensurate rates per band, so the wobble never settles into
            // an obvious loop the ear can latch onto.
            const float wowMod  = (float) (0.7 * std::sin (wowPhase_)  + 0.3 * std::sin (wowPhase2_));
            const float flutMod = (float) (0.6 * std::sin (flutterPhase_) + 0.4 * std::sin (flutterPhase2_));

            wowPhase_      += wowInc;
            wowPhase2_     += wowInc2;
            flutterPhase_  += flutInc;
            flutterPhase2_ += flutInc2;

            const float d = std::max (1.0f, transportSamples_ + wowMod * wowD + flutMod * flutD);

            channels_[0].scratch[(std::size_t) i] = l[i];         // dry, pre-everything
            channels_[0].transport.write (l[i]);
            l[i] = channels_[0].transport.read (d);

            if (r != nullptr)
            {
                channels_[1].scratch[(std::size_t) i] = r[i];
                channels_[1].transport.write (r[i]);
                r[i] = channels_[1].transport.read (d);
            }
        }

        wrapPhases();

        // ---- saturation, head bump, HF loss, mix ----------------------------
        processTape (channels_[0], l, n);
        if (r != nullptr) processTape (channels_[1], r, n);
    }

    void processTape (Channel& ch, float* data, int n) noexcept
    {
        const int factor = ch.ovs.factor();
        float* os = ch.ovs.upsample (data, n);

        for (int i = 0; i < n; ++i)
        {
            const float g = ch.drive.next();
            const float k = ch.comp.next();
            const float b = ch.bias.next();

            for (int s = 0; s < factor; ++s)
            {
                const std::size_t idx = (std::size_t) (i * factor + s);
                os[idx] = harmonic::shapeBiased (harmonic::Curve::Tape, os[idx] * g, b) * k;
            }
        }

        ch.ovs.downsample (data, n);

        for (int i = 0; i < n; ++i)
        {
            float y = ch.dc.process (data[i]);
            y = ch.bump.process (y);
            y = ch.loss.process (y);

            ch.dry.write (ch.scratch[(std::size_t) i]);
            const float dry = ch.dry.readInt (latency_);

            data[i] = (y * ch.wet.next() + dry * ch.dryG.next()) * ch.out.next();
        }
    }

    void wrapPhases() noexcept
    {
        constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
        auto wrap = [] (double& p) { while (p > kTwoPi) p -= kTwoPi; };
        wrap (wowPhase_); wrap (wowPhase2_); wrap (flutterPhase_); wrap (flutterPhase2_);
    }

    Channel channels_[2];

    std::atomic<float> speedIps_ { 15.0f };
    std::atomic<float> driveDb_  {  6.0f };
    std::atomic<float> bias_     {  0.0f };
    std::atomic<float> wow_      { 20.0f };
    std::atomic<float> flutter_  { 20.0f };
    std::atomic<float> bumpDb_   {  2.0f };
    std::atomic<float> mix_      { 100.0f };
    std::atomic<float> outDb_    {  0.0f };

    double sampleRate_ = 44100.0;
    int    maxBlock_   = 0;
    int    osLatency_  = 0;
    int    latency_    = 0;
    float  transportSamples_ = 0.0f;

    double wowPhase_ = 0.0, wowPhase2_ = 0.0;
    double flutterPhase_ = 0.0, flutterPhase2_ = 0.0;
};

} // namespace echojay
