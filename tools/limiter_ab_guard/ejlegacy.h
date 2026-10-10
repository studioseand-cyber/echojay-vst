#pragma once
// ejlegacy.h (session L, 7 Oct 2026): the SHIPPING EchoJay Limiter's per-sample path (EedLimiterProcessor::processBlock,
// transparent mode) ported onto the JUCE-free headers it already uses, so the current limiter renders offline at any
// gain without a plugin build. A port is a claim of fidelity: limiter_v2_core_test proves it against the real Pro Tools
// prints in docs/limiter_ab/renders (residual below -40 dB re signal) whenever those files are present.
#include "../../Source/EedDynamicsCore.h"
#include "../../Source/EJTruePeakInterp.h"
#include <algorithm>
#include <cmath>
namespace echojay::legacy {
struct Limiter
{
    DynamicsCore core; LookaheadDelay delay; TruePeakInterp tpL, tpR; bool truePeak = true; double sr = 48000, lookaheadMs = 2.0, releaseMs = 50.0, ceilingDb = 0.0;
    static constexpr int kMaxWindow = 1024; float winVal[kMaxWindow] {}; int winIdx[kMaxWindow] {}; int winHead = 0, winTail = 0, winN = 0; long long winSample = 0; int windowSamples = 1;
    float wallGain = 1.0f, wallRelCoeff = 0.0f, ceilLin = 1.0f; float inputGain = 1.0f, gainCur = 1.0f; double gainStep = 1.0; int gainStepsLeft = 0;
    inline float windowMaxPush (float v) noexcept
    {
        while (winN > 0) { const int last = (winTail + kMaxWindow - 1) % kMaxWindow; if (winVal[last] <= v) { winTail = last; --winN; } else break; }
        winVal[winTail] = v; winIdx[winTail] = (int) (winSample % 1000000000LL); winTail = (winTail + 1) % kMaxWindow; ++winN;
        const long long oldest = winSample - windowSamples;
        while (winN > 0 && (long long) winIdx[winHead] <= (oldest % 1000000000LL) && winSample >= (long long) windowSamples) { winHead = (winHead + 1) % kMaxWindow; --winN; }
        ++winSample; return winN > 0 ? winVal[winHead] : v;
    }
    void prepare (double sampleRate, double gainDb)
    {
        sr = sampleRate; core.setMode (DynamicsMode::Limit); core.setDetectorMode (DetectorMode::Peak); core.setKneeDb (0.0f); core.setCharacter (CharacterMode::Clean);
        core.setThresholdDb ((float) ceilingDb); core.setSidechainHpfHz (0.0); core.setTruePeak (truePeak);
        core.prepare (sr); core.reset(); tpL.prepare(); tpR.prepare(); ceilLin = (float) std::pow (10.0, ceilingDb / 20.0);
        delay.prepare (sr, 10.0 + 1.0, 2); delay.reset();
        windowSamples = std::max (1, std::min (kMaxWindow - 1, (int) std::lround (lookaheadMs * 0.001 * sr) + 1 + (truePeak ? TruePeakInterp::kDelay : 0)));
        wallRelCoeff = (float) (1.0 - std::exp (-1.0 / (0.001 * releaseMs * sr)));
        const double tpDelayMs = truePeak ? 1000.0 * TruePeakInterp::kDelay / sr : 0.0;
        delay.setDelayMs (lookaheadMs + tpDelayMs);
        core.setAttackMs (std::max (0.05, lookaheadMs / 3.0)); core.setReleaseMs (releaseMs);
        // the input gain: the plugin's SmoothedValue<Multiplicative> eases over 50 ms from where it was (1.0 at prepare)
        // to the restored value - which is what a Pro Tools print shows in its first 50 ms
        inputGain = (float) std::pow (10.0, gainDb / 20.0); gainCur = 1.0f; gainStepsLeft = (int) std::lround (0.05 * sr); gainStep = std::pow ((double) inputGain, 1.0 / (double) gainStepsLeft);
        wallGain = 1.0f; winHead = winTail = winN = 0; winSample = 0;
    }
    int latencySamples() const { return delay.delaySamples(); }
    void process (float* l, float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            if (gainStepsLeft > 0) { gainCur = (float) (gainCur * gainStep); if (--gainStepsLeft == 0) gainCur = inputGain; }
            l[i] *= gainCur; r[i] *= gainCur;
            const float scL = l[i], scR = r[i];
            const float gCore = core.gainForSidechain (scL, scR);
            const float scPeak = truePeak ? std::max (tpL.maxAbs4 (scL), tpR.maxAbs4 (scR)) : std::max (std::abs (scL), std::abs (scR));
            const float wmax = windowMaxPush (scPeak);
            const float ceilDet = truePeak ? ceilLin * 0.98855f : ceilLin;
            const float gTarget = wmax > ceilDet ? ceilDet / wmax : 1.0f;
            if (gTarget < wallGain) wallGain = gTarget; else wallGain += (gTarget - wallGain) * wallRelCoeff;
            const float g = std::min (gCore, wallGain);
            float frame[2] = { l[i], r[i] }; delay.process (frame, 2);
            l[i] = std::max (-ceilLin, std::min (ceilLin, core.shapeCharacter (frame[0] * g)));
            r[i] = std::max (-ceilLin, std::min (ceilLin, core.shapeCharacter (frame[1] * g)));
        }
    }
};
}

