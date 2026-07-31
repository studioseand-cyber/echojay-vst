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

// ---------------------------------------------------------------------------
// The machine (DEVICE_DEPTH_PLAN.md, Harmonic depth). Each mode SCALES what the
// speed, drive, wow, flutter and head-bump controls already do rather than
// replacing them — the knobs keep their meaning, the machine decides how much
// of each vice the mechanics add on top.
//
// Studio is THE NEUTRAL machine: every scale is exactly 1 and both band limits
// are off, so it is bit-for-bit the engine as it shipped and an existing
// session restores the tape it was mixed with.
//
// Index order is FROZEN: it is the value the `mode` param carries and what
// saved state stores.
// ---------------------------------------------------------------------------
enum class TapeMachine { Studio = 0, Vintage = 1, Cassette = 2 };

inline constexpr int kNumTapeMachines = 3;

inline const char* tapeMachineName (TapeMachine m) noexcept
{
    switch (m)
    {
        case TapeMachine::Vintage:  return "vintage";
        case TapeMachine::Cassette: return "cassette";
        case TapeMachine::Studio:
        default:                    return "studio";
    }
}

inline TapeMachine tapeMachineFromIndex (int i) noexcept
{
    if (i <= 0) return TapeMachine::Studio;
    if (i >= kNumTapeMachines - 1) return TapeMachine::Cassette;
    return (TapeMachine) i;
}

struct TapeMachineSpec
{
    float wowScale;        // multiplies the dialled wow depth
    float flutterScale;    // multiplies the dialled flutter depth
    float driveDb;         // extra dB into the curve (compression), level compensated
    float lossScale;       // multiplies the speed-derived HF corner (lower = darker)
    float lossCapHz;       // absolute ceiling on that corner (the cassette band limit)
    float lowCutHz;        // high-pass on the wet path (cassette's narrow bottom); 0 = off
    float bumpScale;       // multiplies the dialled head-bump dB
    float hissScale;       // multiplies the dialled hiss level (cassette is noisiest)
};

inline const TapeMachineSpec& tapeMachineSpec (TapeMachine m) noexcept
{
    // Studio's row is all exactly 1 / off — that IS the neutrality guarantee,
    // pinned by the registry test's defaults-vs-explicit-neutral render.
    static constexpr TapeMachineSpec specs[kNumTapeMachines] = {
        //                 wow   flut  drive  loss   cap       lowcut bump  hiss
        /* studio   */ {  1.0f, 1.0f,  0.0f, 1.0f,  1.0e9f,    0.0f, 1.0f, 1.0f },
        /* vintage  */ {  1.7f, 1.4f,  4.0f, 0.5f,  1.0e9f,    0.0f, 1.15f, 1.6f },
        /* cassette */ {  2.2f, 2.6f,  2.0f, 0.35f, 8000.0f,  60.0f, 0.7f, 3.0f },
    };
    return specs[(int) m >= 0 && (int) m < kNumTapeMachines ? (int) m : 0];
}

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
    static constexpr float kMinHiss      =   0.0f, kMaxHiss      = 100.0f;
    static constexpr float kMinCrosstalk =   0.0f, kMaxCrosstalk = 100.0f;

    // The transport's fixed centre delay, and the most the modulation may move
    // it. The centre must exceed the sum of the depths or a deep wow would read
    // past the write head into the future.
    static constexpr float kTransportMs  = 2.5f;
    static constexpr float kMaxWowMs     = 1.0f;
    static constexpr float kMaxFlutterMs = 0.2f;

    // The deepest each machine can scale the wobble — the transport buffer is
    // sized for these, so no machine/speed/depth combination can ever read past
    // the write head.
    static constexpr float kMaxMachineWowScale     = 2.2f;
    static constexpr float kMaxMachineFlutterScale = 2.6f;

    // Hiss at 100% on the studio machine, linear (about -50 dBFS before the
    // machine's own hissScale and the playback filters shape it).
    static constexpr float kHissLevel = 0.0032f;

    // Crosstalk at 100%: 30% of the opposite channel blended in — audible as
    // glue and a narrower image, never as a collapse to mono.
    static constexpr float kMaxCrosstalkBlend = 0.30f;

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

        // Sized for the deepest wobble ANY machine can ask for (the machine
        // scales multiply the dialled depths), times the speed scale's own 2x.
        const int transportMax = (int) std::ceil (transportSamples_
                                                  + (kMaxWowMs * kMaxMachineWowScale
                                                     + kMaxFlutterMs * kMaxMachineFlutterScale) * 2.0f
                                                    * 0.001f * (float) sampleRate_) + 4;
        maxTransportDelay_ = (float) (transportMax - 4);

        // Hiss gate: snaps open in a few ms, lets go over ~150 ms, so the noise
        // arrives with the material and breathes out after it rather than
        // gating on and off audibly.
        hissAttack_  = 1.0f - (float) std::exp (-1.0 / (0.003 * sampleRate_));
        hissRelease_ = 1.0f - (float) std::exp (-1.0 / (0.150 * sampleRate_));

        int seed = 0;
        for (auto& ch : channels_)
        {
            ch.ovs.prepare (kOversampling, maxBlock_);
            ch.transport.prepare (transportMax);
            ch.dry.prepare (latency_ + 4);
            ch.dc.prepare (sampleRate_);
            ch.bump.prepare (sampleRate_);
            ch.loss.prepare (sampleRate_);
            ch.lowCut.prepare (sampleRate_);
            ch.scratch.assign ((std::size_t) maxBlock_, 0.0f);

            // Different seed per channel: correlated hiss narrows the image the
            // way real tape noise never does.
            ch.rng = 0x9E3779B9u * (unsigned) (++seed);
        }

        inputLevel_.prepare (sampleRate_);

        applySpeedDependentFilters();
        reset();
    }

    void reset() noexcept
    {
        // The machine's extra drive is part of the operating point, so the
        // smoothers snap to the same effective values the process loop chases.
        const auto& spec = tapeMachineSpec (getMachine());
        const float g = std::pow (10.0f, (driveDb_.load() + spec.driveDb) * 0.05f);

        inputLevel_.reset();
        inputLevelTap_.set (0.0f);
        transportTap_.set (0.0f);

        for (auto& ch : channels_)
        {
            ch.ovs.reset();
            ch.transport.reset();
            ch.dry.reset();
            ch.dc.reset();
            ch.bump.reset();
            ch.loss.reset();
            ch.lowCut.reset();
            ch.hissEnv = 0.0f;

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
    void setMachine   (TapeMachine m) noexcept
    {
        machine_.store ((int) m);
        // The machine reshapes the bump and the HF corner, so the filters need
        // rebuilding the same way a speed change does.
        applySpeedDependentFilters();
    }
    void setHiss      (float pc) noexcept { hiss_.store (pc); }
    void setCrosstalk (float pc) noexcept { crosstalk_.store (pc); }

    float getSpeedIps()   const noexcept { return speedIps_.load(); }
    float getDriveDb()    const noexcept { return driveDb_.load(); }
    float getBias()       const noexcept { return bias_.load(); }
    float getWow()        const noexcept { return wow_.load(); }
    float getFlutter()    const noexcept { return flutter_.load(); }
    float getHeadBumpDb() const noexcept { return bumpDb_.load(); }
    float getMixPercent() const noexcept { return mix_.load(); }
    float getOutputDb()   const noexcept { return outDb_.load(); }
    TapeMachine getMachine() const noexcept { return tapeMachineFromIndex (machine_.load()); }
    float getHiss()       const noexcept { return hiss_.load(); }
    float getCrosstalk()  const noexcept { return crosstalk_.load(); }

    // The frequencies the current speed AND machine imply. Exposed so the
    // editor can show what the dials are actually doing rather than a number.
    float headBumpHz() const noexcept { return bumpHzFor (speedIps_.load()); }
    float hfLossHz()   const noexcept
    {
        const auto& spec = tapeMachineSpec (getMachine());
        return std::min (lossHzFor (speedIps_.load(), sampleRate_) * spec.lossScale,
                         spec.lossCapHz);
    }

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

        inputLevelTap_.set (inputLevel_.push (
            std::max (harmonic::blockPeak (left, numSamples),
                      harmonic::blockPeak (right, numSamples)), numSamples));

        const int maxChunk = std::max (1, channels_[0].ovs.maxBlockSize());

        for (int offset = 0; offset < numSamples; offset += maxChunk)
        {
            const int n = std::min (maxChunk, numSamples - offset);
            processChunk (left + offset, right != nullptr ? right + offset : nullptr, n);
        }
    }

    // ---- visualisation (message thread) -----------------------------------
    // The level entering the shaper, 0..1, before the drive gain — the domain
    // the WaveshaperView plots in.
    float inputLevel() const noexcept { return inputLevelTap_.get(); }

    // Where the transport currently sits, -1..+1 around its centre delay. This
    // is the wow and flutter themselves: the number the modulated delay is
    // actually reading at, not a re-synthesised copy of the LFOs, so the
    // indicator cannot drift out of step with what is being heard.
    float transportOffset() const noexcept { return transportTap_.get(); }

private:
    struct Channel
    {
        harmonic::Oversampler ovs;
        harmonic::DelayLine   transport;   // wow/flutter
        harmonic::DelayLine   dry;
        harmonic::DCBlock     dc;
        harmonic::PeakBiquad  bump;
        harmonic::OnePoleLP   loss;
        harmonic::OnePoleLP   lowCut;      // cassette band limit (subtractive HP)

        std::vector<float> scratch;

        harmonic::SmoothedValue drive, comp, bias, wet, dryG, out;

        // Hiss: a per-channel noise source and the input envelope its gate rides.
        unsigned rng     = 0x9E3779B9u;
        float    hissEnv = 0.0f;
    };

    void applySpeedDependentFilters() noexcept
    {
        const auto& spec = tapeMachineSpec (getMachine());
        const float ips  = speedIps_.load();
        const float hz   = bumpHzFor (ips);
        const float db   = std::min (bumpDb_.load() * spec.bumpScale, kMaxBumpDb + 3.0f);
        const float loss = std::min (lossHzFor (ips, sampleRate_) * spec.lossScale,
                                     spec.lossCapHz);

        for (auto& ch : channels_)
        {
            // Q 1.0: wide enough to read as weight rather than as a resonance.
            ch.bump.setPeak (hz, 1.0f, db);
            ch.loss.setCutoff (loss);
            // The cassette's narrow bottom. Set unconditionally (it is 0 → the
            // filter opens to its 1 Hz floor) but APPLIED only when the machine
            // asks for it — see processTape — so studio stays bit-exact.
            ch.lowCut.setCutoff (spec.lowCutHz);
        }
    }

    void processChunk (float* l, float* r, int n) noexcept
    {
        const auto& spec   = tapeMachineSpec (getMachine());
        const float coeff  = harmonic::blockCoefficient (sampleRate_, n, 0.020);
        // The machine's extra drive rides the knob's, and the compensation sees
        // the same effective value — so a softer machine compresses more at the
        // same DRIVE setting without also getting louder.
        const float driveG = std::pow (10.0f, (driveDb_.load() + spec.driveDb) * 0.05f);
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
        const float wowD   = wow_.load()     * 0.01f * kMaxWowMs     * scale * spec.wowScale
                             * 0.001f * (float) sampleRate_;
        const float flutD  = flutter_.load() * 0.01f * kMaxFlutterMs * scale * spec.flutterScale
                             * 0.001f * (float) sampleRate_;

        // Crosstalk narrows toward the other channel symmetrically, so the mono
        // sum is untouched by construction; the dry path is captured before it,
        // so MIX still blends against the true input.
        const float ct = std::clamp (crosstalk_.load(), kMinCrosstalk, kMaxCrosstalk)
                         * 0.01f * kMaxCrosstalkBlend;

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

            const float offset = wowMod * wowD + flutMod * flutD;
            const float d = std::clamp (transportSamples_ + offset, 1.0f, maxTransportDelay_);

            // Published on the LAST sample of the chunk, normalised against the
            // deepest excursion the device can produce rather than against the
            // current depth — so turning WOW down moves the needle less, which
            // is the thing the indicator is there to show.
            if (i == n - 1)
            {
                constexpr float kMaxOffsetMs = kMaxWowMs + kMaxFlutterMs;
                const float full = kMaxOffsetMs * 2.0f * 0.001f * (float) sampleRate_;
                transportTap_.set (full > 0.0f
                                   ? std::clamp (offset / full, -1.0f, 1.0f) : 0.0f);
            }

            channels_[0].scratch[(std::size_t) i] = l[i];         // dry, pre-everything
            channels_[0].transport.write (l[i]);
            l[i] = channels_[0].transport.read (d);

            if (r != nullptr)
            {
                channels_[1].scratch[(std::size_t) i] = r[i];
                channels_[1].transport.write (r[i]);
                r[i] = channels_[1].transport.read (d);

                // The head bleed, applied where it happens on a real machine —
                // at the heads, before the tape's own saturation.
                if (ct > 0.0f)
                {
                    const float li = l[i], ri = r[i];
                    l[i] = li + ct * (ri - li);
                    r[i] = ri + ct * (li - ri);
                }
            }
        }

        wrapPhases();

        // ---- saturation, head bump, HF loss, mix ----------------------------
        processTape (channels_[0], l, n);
        if (r != nullptr) processTape (channels_[1], r, n);
    }

    void processTape (Channel& ch, float* data, int n) noexcept
    {
        const auto& spec = tapeMachineSpec (getMachine());

        // Hiss level for this chunk. Zero skips the whole path — the RNG is not
        // even advanced — which is what keeps hiss 0 bit-exact.
        const float hissAmp = std::clamp (hiss_.load(), kMinHiss, kMaxHiss)
                              * 0.01f * kHissLevel * spec.hissScale;
        const bool  lowCut  = spec.lowCutHz > 0.0f;

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

            // Tape noise lives ON the tape, so it is added before the playback
            // filters (bump, loss) and shares their tone. Gated to the DRY
            // input's envelope: it swells with the material and breathes out
            // after it, instead of humming through the silences.
            if (hissAmp > 0.0f)
            {
                const float ax = std::fabs (ch.scratch[(std::size_t) i]);
                ch.hissEnv += (ax > ch.hissEnv ? hissAttack_ : hissRelease_)
                              * (ax - ch.hissEnv);

                ch.rng = ch.rng * 1664525u + 1013904223u;
                const float white = (float) (ch.rng >> 8) * (1.0f / 8388608.0f) - 1.0f;
                const float gate  = std::min (1.0f, ch.hissEnv * 25.0f);
                y += white * hissAmp * gate;
            }

            y = ch.bump.process (y);
            y = ch.loss.process (y);

            // The cassette's narrow bottom: a gentle high-pass, applied only
            // when the machine asks for one so the other machines stay exact.
            if (lowCut) y -= ch.lowCut.process (y);

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
    std::atomic<int>   machine_  { (int) TapeMachine::Studio };
    std::atomic<float> hiss_     {  0.0f };
    std::atomic<float> crosstalk_{  0.0f };

    double sampleRate_ = 44100.0;
    int    maxBlock_   = 0;
    int    osLatency_  = 0;
    int    latency_    = 0;
    float  transportSamples_ = 0.0f;
    float  maxTransportDelay_ = 1.0f;   // real value set in prepare()
    float  hissAttack_  = 0.0f;
    float  hissRelease_ = 0.0f;

    double wowPhase_ = 0.0, wowPhase2_ = 0.0;
    double flutterPhase_ = 0.0, flutterPhase2_ = 0.0;

    viz::FloatTap          inputLevelTap_ { 0.0f };
    harmonic::PeakFollower inputLevel_;
    viz::FloatTap          transportTap_  { 0.0f };
};

} // namespace echojay
