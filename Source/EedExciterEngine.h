/*
    EedExciterEngine.h  —  the DSP behind "EchoJay Exciter".

    A face on EedHarmonicCore: split the band, shape only what is ABOVE the split,
    put it back. That is the whole difference between an exciter and a saturator —
    the low end stays clean, so you can add air and edge without also making the
    bass woolly, which is what happens when you drive a full-band saturator to get
    the same top-end lift.

        in -> upsample -> low = LP(x), high = x - low
                       -> high crossfaded toward shape(high * g)
                       -> low + high
           -> downsample -> dry/wet (dry delay-matched) -> output

    SUBTRACTIVE SPLIT. The high band is defined as `x - low`, not as a separate
    high-pass. That makes reconstruction EXACT: at amount 0 the two bands sum back
    to the input sample for sample, with no crossover phase error to hear. Two
    independent filters would only sum flat if their phase responses happened to
    agree, and at the crossover they do not.

    The split runs INSIDE the oversampled domain so the shaper's products are
    filtered by the same anti-imaging filters as everything else; splitting at
    base rate and oversampling only the high band would put the crossover on one
    side of the latency and the low band on the other.

    JUCE-free, header-only, same discipline as EqEngine.
*/

#pragma once

#include "EedHarmonicCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace echojay
{

class ExciterEngine
{
public:
    // Advertised ranges — the SAME numbers the ParamSchema publishes.
    static constexpr float kMinFreqHz =  500.0f, kMaxFreqHz = 12000.0f;
    static constexpr float kMinAmount =    0.0f, kMaxAmount =   100.0f;
    static constexpr float kMinOutDb  =  -24.0f, kMaxOutDb  =    24.0f;

    static constexpr int kOversampling = 4;

    // Only the two modes the device advertises. Index order is frozen: it is the
    // value the `mode` param carries and what saved state stores.
    static harmonic::Curve curveForMode (int modeIndex) noexcept
    {
        return modeIndex >= 1 ? harmonic::Curve::Tape : harmonic::Curve::Tube;
    }

    ExciterEngine() = default;

    void prepare (double sampleRate, int maxBlockSize)
    {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        maxBlock_   = std::max (1, maxBlockSize);
        latency_    = harmonic::Oversampler::latencyForFactor (kOversampling);

        const double osRate = sampleRate_ * (double) kOversampling;

        for (auto& ch : channels_)
        {
            ch.ovs.prepare (kOversampling, maxBlock_);
            ch.dry.prepare (latency_ + 4);
            ch.dc.prepare (osRate);
            // The splitter runs at the OVERSAMPLED rate, so it is prepared with
            // that rate; handing it the base rate would put the crossover four
            // times too low.
            ch.split1.prepare (osRate);
            ch.split2.prepare (osRate);
            ch.scratch.assign ((std::size_t) maxBlock_, 0.0f);
        }

        applyCrossover();
        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : channels_)
        {
            ch.ovs.reset();
            ch.dry.reset();
            ch.dc.reset();
            ch.split1.reset();
            ch.split2.reset();

            ch.amount.snap (amount_.load() * 0.01f);
            ch.wet   .snap (mix_.load() * 0.01f);
            ch.dryG  .snap (1.0f - mix_.load() * 0.01f);
            ch.out   .snap (std::pow (10.0f, outDb_.load() * 0.05f));
        }
    }

    int latencySamples() const noexcept { return latency_; }

    // ---- parameters (message thread) --------------------------------------
    void setFreqHz (float hz) noexcept
    {
        freqHz_.store (std::clamp (hz, kMinFreqHz, kMaxFreqHz));
        applyCrossover();
    }
    void setAmount    (float a)  noexcept { amount_.store (a); }
    void setMode      (int m)    noexcept { mode_.store (m <= 0 ? 0 : 1); }
    void setMixPercent(float pc) noexcept { mix_.store (pc); }
    void setOutputDb  (float db) noexcept { outDb_.store (db); }

    float getFreqHz()     const noexcept { return freqHz_.load(); }
    float getAmount()     const noexcept { return amount_.load(); }
    int   getMode()       const noexcept { return mode_.load(); }
    float getMixPercent() const noexcept { return mix_.load(); }
    float getOutputDb()   const noexcept { return outDb_.load(); }

    // The drive the high band sees at a given amount. Starts at 2x so even a
    // small amount is doing something audible, and reaches 8x at the top.
    static float driveForAmount (float amount01) noexcept
    {
        return 2.0f + 6.0f * std::clamp (amount01, 0.0f, 1.0f);
    }

    // ---- audio thread ------------------------------------------------------
    void process (float* left, float* right, int numSamples) noexcept
    {
        if (numSamples <= 0 || left == nullptr) return;

        const int maxChunk = std::max (1, channels_[0].ovs.maxBlockSize());

        for (int offset = 0; offset < numSamples; offset += maxChunk)
        {
            const int n = std::min (maxChunk, numSamples - offset);
            const float coeff = harmonic::blockCoefficient (sampleRate_, n, 0.020);

            processChunk (channels_[0], left + offset, n, coeff);
            if (right != nullptr) processChunk (channels_[1], right + offset, n, coeff);
        }
    }

private:
    struct Channel
    {
        harmonic::Oversampler ovs;
        harmonic::DelayLine   dry;
        harmonic::DCBlock     dc;
        harmonic::OnePoleLP   split1, split2;   // cascaded: a 12 dB/oct split

        std::vector<float> scratch;

        harmonic::SmoothedValue amount, wet, dryG, out;
    };

    void applyCrossover() noexcept
    {
        const float hz = freqHz_.load();
        for (auto& ch : channels_)
        {
            ch.split1.setCutoff (hz);
            ch.split2.setCutoff (hz);
        }
    }

    void processChunk (Channel& ch, float* data, int n, float coeff) noexcept
    {
        const float amt  = std::clamp (amount_.load() * 0.01f, 0.0f, 1.0f);
        const float wet  = std::clamp (mix_.load() * 0.01f, 0.0f, 1.0f);
        const float outG = std::pow (10.0f, outDb_.load() * 0.05f);

        ch.amount.startBlock (amt, n, coeff);
        ch.wet   .startBlock (wet, n, coeff);
        ch.dryG  .startBlock (1.0f - wet, n, coeff);
        ch.out   .startBlock (outG, n, coeff);

        for (int i = 0; i < n; ++i) ch.scratch[(std::size_t) i] = data[i];

        const harmonic::Curve curve = curveForMode (mode_.load());
        const int factor = ch.ovs.factor();
        float* os = ch.ovs.upsample (data, n);

        for (int i = 0; i < n; ++i)
        {
            const float a = ch.amount.next();
            const float g = driveForAmount (a);
            // 1/g, not the full drive compensation: the shaped high band has to
            // come back at the SAME level as the clean one, so that `amount`
            // controls harmonic content and not brightness-by-volume.
            const float k = 1.0f / g;

            for (int s = 0; s < factor; ++s)
            {
                const std::size_t idx = (std::size_t) (i * factor + s);
                const float x = os[idx];

                const float low  = ch.split2.process (ch.split1.process (x));
                const float high = x - low;

                const float shaped = harmonic::shape (curve, high * g) * k;

                // Only what the shaper ADDS is DC-blocked, not the signal. Tube
                // mode is asymmetric and does emit DC, but running the blocker
                // across the whole path would put its (tiny) phase shift on the
                // clean bands too, and then amount 0 would no longer reconstruct
                // the input exactly — which is the one property this topology
                // exists to give.
                const float added = ch.dc.process (a * (shaped - high));

                os[idx] = low + high + added;
            }
        }

        ch.ovs.downsample (data, n);

        for (int i = 0; i < n; ++i)
        {
            const float y = data[i];

            ch.dry.write (ch.scratch[(std::size_t) i]);
            const float dry = ch.dry.readInt (latency_);

            data[i] = (y * ch.wet.next() + dry * ch.dryG.next()) * ch.out.next();
        }
    }

    Channel channels_[2];

    std::atomic<float> freqHz_ { 3000.0f };
    std::atomic<float> amount_ {   50.0f };
    std::atomic<int>   mode_   {      0  };
    std::atomic<float> mix_    {  100.0f };
    std::atomic<float> outDb_  {    0.0f };

    double sampleRate_ = 44100.0;
    int    maxBlock_   = 0;
    int    latency_    = 0;
};

} // namespace echojay
