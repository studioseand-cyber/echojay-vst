/*
    EedPitchEngine.h  —  the pitch DETECTOR behind "EchoJay Pitch"
    (PITCH_CORRECTION_SPEC.md §2.1, build phase P0).

    Deliberately JUCE-free, like EqEngine/KeyEngine, so the whole detection
    pipeline unit-tests under plain g++ (test/pitch_engine_test.cpp).

    P0 SCOPE. Detection only: this engine READS audio and publishes f0, a
    confidence, a voiced/unvoiced flag and an octave-guard counter. It never
    writes a sample. Shifting (PSOLA), the decision stage and the retune
    envelope are later phases and are not here.

    THE ESTIMATOR is YIN (de Cheveigné & Kawahara 2002), the published
    difference-function family:

      1. difference function d(tau) over an integration window W,
      2. cumulative mean normalised difference d'(tau),
      3. absolute threshold: the FIRST dip of d' below 0.15, descended to its
         local minimum (falling back to the global minimum when nothing dips),
      4. parabolic interpolation of d' around the chosen lag.

    Aperiodicity = d'(tau*) at the chosen lag; confidence = 1 - aperiodicity.
    A frame is UNVOICED when the frame RMS is under the silence gate or the
    best aperiodicity is above the voicing threshold — the caller must leave
    such frames untouched (correcting a consonant is what makes cheap
    correctors sound cheap).

    THE ANALYSIS WINDOW IS voice_type (spec §2.1): each of the five settings
    fixes a search range [fMin, fMax]; the window is ~2.5 periods of fMin, so
    a soprano window is a fraction of a bass one. Input is decimated (after a
    4th-order Butterworth anti-alias) to a per-type analysis rate so the bass
    types don't cost O(fs^2) on the audio thread.

    OCTAVE ERRORS ARE THE ENEMY (spec, verbatim). YIN halves and doubles under
    vibrato and on breathy onsets. The guard here is the spec's: a short median
    over recent estimates plus a continuity bias toward the previous f0 —
    implemented as candidate re-scoring over a HARMONIC LATTICE (tau/3, tau/2,
    tau*2/3, tau, tau*3/2, tau*2, tau*3, each descended to its local d'
    minimum, penalised by octave distance from the recent f0), then a
    median-of-3 over the survivors. The lattice is wider than octaves because
    measured on real singing the third-harmonic confusion is as common as the
    octave one; see the block above kGuardLattice. Every hop where the guard
    changes the raw YIN answer increments guardFires: if it fires constantly
    the WINDOW is wrong for the material (wrong voice_type), not the guard.

    WHAT THE GUARD CANNOT REACH. About 4-7% of the surviving >600 cent errors
    are high-confidence and mid-phrase (PITCH_P0_VALIDATION.md §2). That is the
    detector's floor, roughly one every 12-18 seconds, and no threshold removes
    it. Anything downstream that hears a glitch at that rate should suspect
    detection before it suspects itself.

    THREADING. process() is audio-thread only: no allocation, no locks, no
    logging; all buffers are pre-allocated in prepare() at the worst case any
    voice_type can ask for. Results are published through individual relaxed
    atomics — a debug readout may tear across fields, which is harmless.
    Setters are safe from the message thread.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace echojay
{

// ---------------------------------------------------------------------------
// One published detection result — assembled from atomics, safe to poll from
// any thread. Counters are cumulative since the last resetStats().
// ---------------------------------------------------------------------------
struct PitchReading
{
    bool     voiced       = false;
    float    f0Hz         = 0.0f;   // 0 while unvoiced
    float    confidence   = 0.0f;   // 1 - aperiodicity, clamped to 0..1
    float    aperiodicity = 1.0f;   // d'(tau*) at the chosen lag
    float    rmsDb        = -120.0f;

    uint32_t guardFires   = 0;      // hops where the octave guard overrode raw YIN
    uint32_t voicedHops   = 0;
    uint32_t totalHops    = 0;      // analysed hops (excludes warm-up)
};

// ---------------------------------------------------------------------------
class PitchEngine
{
public:
    PitchEngine() = default;

    // ---- voice_type (spec §5): the search range IS the control ------------
    // Order mirrors the ParamSchema's choices list exactly.
    enum VoiceType
    {
        kSoprano = 0, kAltoTenor, kLowMale, kInstrument, kBass,
        kNumVoiceTypes
    };

    struct VoiceRange
    {
        const char* id;
        float fMinHz, fMaxHz;
        float targetAnalysisHz;   // decimation target; actual rate = fs/decim
    };

    static const VoiceRange& voiceRange (int type) noexcept
    {
        // Ranges follow the Antares-class conventions for the same five
        // settings; targets keep the O(W * tauMax) hop cost bounded on the
        // low types without costing cents resolution on the high ones.
        static const VoiceRange table[kNumVoiceTypes] = {
            { "soprano",    180.0f, 1400.0f, 48000.0f },
            { "alto_tenor",  80.0f,  900.0f, 24000.0f },
            { "low_male",    55.0f,  500.0f, 12000.0f },
            { "instrument",  50.0f, 2000.0f, 24000.0f },
            { "bass",        25.0f,  500.0f, 12000.0f },
        };
        return table[type < 0 ? 0 : (type >= kNumVoiceTypes ? kNumVoiceTypes - 1 : type)];
    }

    // ---- tracking: how strict the detector is before it calls a frame pitched
    // Order mirrors the ParamSchema's choices list exactly.
    enum Tracking { kRelaxed = 0, kNormal, kTight, kNumTracking };

    // Minimum CONFIDENCE (1 - aperiodicity) for a frame to be tracked. These
    // three numbers are the taste decision made dialable, and each is measured
    // rather than chosen (PITCH_P0_VALIDATION.md §5.3, §6):
    //
    //   relaxed 0.60 - the pre-gate behaviour. Keeps every frame P0 shipped
    //                  with, for breathy or quiet sources where losing frames
    //                  costs more than the occasional bad one.
    //   normal  0.75 - the knee. Removes 77% / 64% of residual >600 cent jumps
    //                  on the male take at alto_tenor / low_male for 13% of
    //                  tracked frames; 0.70->0.75 buys 11 points of reduction
    //                  for 4.6 of tracking, 0.75->0.80 buys 8 for 6.
    //   tight   0.80 - the CEILING SET BY GAP LENGTH, not by residual. It is
    //                  the highest threshold at which fewer than 2% of
    //                  mid-phrase gaps exceed 100 ms (1.6% / 1.1% / 0%, p95 =
    //                  59 / 55 / 24 ms). At 0.82 that doubles to ~2.7%, which
    //                  starts handing the retune envelope holes it has to
    //                  reason about.
    static constexpr float kTrackingConfidence[kNumTracking] = { 0.60f, 0.75f, 0.80f };

    static float trackingConfidence (int t) noexcept
    {
        return kTrackingConfidence[t < 0 ? 0 : (t >= kNumTracking ? kNumTracking - 1 : t)];
    }

    // ---- tuning constants --------------------------------------------------
    static constexpr float kYinThreshold        = 0.15f;  // the absolute-threshold dip
    // Structural floor: below this a frame is not periodic enough to analyse,
    // whatever `tracking` says. Equal to the relaxed floor by construction, so
    // relaxed reproduces the pre-gate behaviour exactly.
    static constexpr float kMaxAperiodicity     = 0.40f;
    static constexpr float kSilenceGateDb       = -55.0f; // frame RMS under this = unvoiced
    static constexpr float kOctaveBiasPerOct    = 0.12f;  // d' handicap per octave away from recent f0
    static constexpr float kSubMultipleEvidence = 0.02f;  // d' advantage a DOWNWARD claim must show
    static constexpr float kUpClaimMargin       = 0.01f;  // d' advantage an UPWARD claim must show
    static constexpr float kWindowPeriods       = 2.5f;   // of fMin (spec: 2-3)
    static constexpr float kHopSeconds          = 128.0f / 48000.0f;  // spec: 128 @ 48k
    static constexpr float kContinuityMaxS      = 0.25f;  // recent-f0 bias goes stale after this
    static constexpr float kHistoryClearS       = 0.5f;   // unvoiced this long clears the median

    // ---- lifecycle (audio stopped) ----------------------------------------
    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs_ = sampleRate > 0.0 ? sampleRate : 48000.0;

        // Worst case across ALL voice types, so a type switch mid-playback
        // never allocates on the audio thread.
        int maxFrame = 0, maxTau = 0;
        for (int t = 0; t < kNumVoiceTypes; ++t)
        {
            const Config c = deriveConfig (t);
            maxFrame = std::max (maxFrame, c.frameLen);
            maxTau   = std::max (maxTau,   c.tauMax);
        }
        frame_.assign ((size_t) maxFrame, 0.0f);
        diff_.assign  ((size_t) maxTau + 2, 0.0f);
        cmndf_.assign ((size_t) maxTau + 2, 1.0f);

        ringBits_ = 1;
        while ((1 << ringBits_) < maxFrame + 64) ++ringBits_;
        ring_.assign ((size_t) (1 << ringBits_), 0.0f);
        ringMask_ = (uint32_t) ((1 << ringBits_) - 1);

        curType_ = -1;                         // force applyConfig on first block
        publishUnvoiced (-120.0f);
    }

    // ---- parameters (message thread) --------------------------------------
    void setVoiceType (int t) noexcept
    {
        voiceTypeReq_.store (t < 0 ? 0 : (t >= kNumVoiceTypes ? kNumVoiceTypes - 1 : t),
                             std::memory_order_relaxed);
    }
    int getVoiceType() const noexcept { return voiceTypeReq_.load (std::memory_order_relaxed); }

    void setTracking (int t) noexcept
    {
        trackingReq_.store (t < 0 ? 0 : (t >= kNumTracking ? kNumTracking - 1 : t),
                            std::memory_order_relaxed);
    }
    int getTracking() const noexcept { return trackingReq_.load (std::memory_order_relaxed); }

    // The confidence a frame must reach RIGHT NOW to be tracked - published so
    // the debug readout can show the gate next to the value being gated.
    float trackingFloor() const noexcept { return trackingConfidence (getTracking()); }

    // Zero the guard/frame counters (the debug readout's RESET, and the AI's
    // way to start a clean octave-error measurement pass).
    void resetStats() noexcept
    {
        guardFires_.store (0, std::memory_order_relaxed);
        voicedHops_.store (0, std::memory_order_relaxed);
        totalHops_.store  (0, std::memory_order_relaxed);
    }

    // ---- audio thread ------------------------------------------------------
    // READS ONLY. `r` may be null (mono); stereo is analysed as the mid sum.
    void process (const float* l, const float* r, int n) noexcept
    {
        const int vt = voiceTypeReq_.load (std::memory_order_relaxed);
        if (vt != curType_) applyConfig (vt);

        for (int i = 0; i < n; ++i)
        {
            float m = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
            if (decim_ > 1) m = aa2_.process (aa1_.process (m));

            if (++decimPhase_ >= decim_)
            {
                decimPhase_ = 0;
                ring_[(uint32_t) wr_ & ringMask_] = m;
                ++wr_;
                if (++sinceHop_ >= hopA_)
                {
                    sinceHop_ = 0;
                    analyseHop();
                }
            }
        }
    }

    // ---- results (any thread) ---------------------------------------------
    PitchReading getReading() const noexcept
    {
        PitchReading o;
        o.voiced       = outVoiced_.load (std::memory_order_relaxed);
        o.f0Hz         = outF0_.load (std::memory_order_relaxed);
        o.confidence   = outConf_.load (std::memory_order_relaxed);
        o.aperiodicity = outAp_.load (std::memory_order_relaxed);
        o.rmsDb        = outRmsDb_.load (std::memory_order_relaxed);
        o.guardFires   = guardFires_.load (std::memory_order_relaxed);
        o.voicedHops   = voicedHops_.load (std::memory_order_relaxed);
        o.totalHops    = totalHops_.load (std::memory_order_relaxed);
        return o;
    }

    // Introspection for tests and the readout. These read the ACTIVE config,
    // so they are only meaningful once process() has run at least once (the
    // first block is what applies a pending voice_type).
    double analysisRate() const noexcept { return fsA_; }
    int    windowLength() const noexcept { return W_; }
    int    hopLength()    const noexcept { return hopA_; }

    // The INPUT-domain hop for a voice type at the prepared sample rate, so
    // an offline driver can feed exactly one analysis hop per call and attach
    // a timestamp to each published reading. Derived from the same
    // deriveConfig() the audio path uses rather than recomputed, so a probe
    // cannot drift from the engine it is measuring. Valid straight after
    // prepare(), with no dummy processing needed to force a config.
    int inputHopLength (int voiceType) const noexcept
    {
        const Config c = deriveConfig (voiceType);
        return c.hopA * c.decim;
    }

    // ---- note formatting (shared by editor and tests) ---------------------
    static float hzToMidi (float hz) noexcept
    {
        return 69.0f + 12.0f * std::log2 (hz / 440.0f);
    }

    static float centsBetween (float a, float b) noexcept
    {
        return 1200.0f * std::log2 (a / b);
    }

    // "A3" + the deviation in cents from that note's centre. buf >= 8 chars.
    static void noteName (float hz, char* buf, int bufLen, float* centsOut = nullptr)
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };
        if (! (hz > 0.0f)) { std::snprintf (buf, (size_t) bufLen, "-"); if (centsOut) *centsOut = 0; return; }
        const float midi = hzToMidi (hz);
        const int nearest = (int) std::lround (midi);
        const int pc = ((nearest % 12) + 12) % 12;
        std::snprintf (buf, (size_t) bufLen, "%s%d", names[pc], nearest / 12 - 1);
        if (centsOut) *centsOut = (midi - (float) nearest) * 100.0f;
    }

private:
    // ---- the guard's candidate lattice ------------------------------------
    // tau * num / den. den > num shortens the lag (reports a HIGHER f0, an
    // "up" claim); num > den lengthens it (reports a LOWER f0, a "down" claim
    // that must show evidence). Ordered low-tau to high-tau purely for
    // readability; the search takes the best accepted candidate, not the first.
    struct GuardCandidate { int num, den; };

    static constexpr GuardCandidate kGuardLattice[] = {
        { 1, 3 },   // tau/3     -> 3x f0     up   (recovers a 3rd-harmonic lock)
        { 1, 2 },   // tau/2     -> 2x f0     up   (recovers an octave lock)
        { 2, 3 },   // tau*2/3   -> 1.5x f0   up   (recovers a fifth lock)
        { 3, 2 },   // tau*3/2   -> f0 / 1.5  down (needs evidence)
        { 2, 1 },   // tau*2     -> f0 / 2    down (needs evidence)
        { 3, 1 },   // tau*3     -> f0 / 3    down (needs evidence)
    };

    // ---- per-voice-type configuration -------------------------------------
    struct Config
    {
        int decim = 1, tauMin = 2, tauMax = 3, W = 0, frameLen = 0, hopA = 128;
        double fsA = 48000.0;
    };

    Config deriveConfig (int type) const
    {
        const VoiceRange& vr = voiceRange (type);
        Config c;
        c.decim  = std::max (1, (int) std::lround (fs_ / vr.targetAnalysisHz));
        c.fsA    = fs_ / c.decim;
        c.tauMax = (int) std::ceil (c.fsA / vr.fMinHz);
        c.tauMin = std::max (2, (int) std::floor (c.fsA / vr.fMaxHz));
        if (c.tauMin >= c.tauMax) c.tauMin = c.tauMax - 1;
        c.W        = (int) std::lround (kWindowPeriods * c.fsA / vr.fMinHz);
        c.frameLen = c.W + c.tauMax + 2;          // +2: parabolic needs tau+1
        c.hopA     = std::max (16, (int) std::lround (c.fsA * kHopSeconds));
        return c;
    }

    void applyConfig (int type) noexcept
    {
        curType_ = type;
        const Config c = deriveConfig (type);
        decim_ = c.decim; fsA_ = c.fsA;
        tauMin_ = c.tauMin; tauMax_ = c.tauMax;
        W_ = c.W; frameLen_ = c.frameLen; hopA_ = c.hopA;

        // 4th-order Butterworth AA at 0.4 * fsA (only used when decimating).
        aa1_.makeLowpass (fs_, 0.4 * fsA_, 0.54119610);
        aa2_.makeLowpass (fs_, 0.4 * fsA_, 1.30656296);
        aa1_.resetState(); aa2_.resetState();

        decimPhase_ = 0; sinceHop_ = 0; wr_ = 0;

        histCount_ = 0;
        hopsSinceVoiced_ = 1 << 24;
        continuityMaxHops_  = std::max (1, (int) (kContinuityMaxS * fsA_ / hopA_));
        historyClearHops_   = std::max (1, (int) (kHistoryClearS  * fsA_ / hopA_));

        publishUnvoiced (-120.0f);
    }

    // ---- the per-hop analysis (audio thread) ------------------------------
    void analyseHop() noexcept
    {
        if (wr_ < (uint64_t) frameLen_) return;          // warm-up after a (re)config

        // Newest frameLen_ samples, chronological.
        const uint64_t start = wr_ - (uint64_t) frameLen_;
        for (int k = 0; k < frameLen_; ++k)
            frame_[(size_t) k] = ring_[(uint32_t) (start + (uint64_t) k) & ringMask_];

        totalHops_.fetch_add (1, std::memory_order_relaxed);

        // Silence gate on frame RMS.
        double acc = 0.0;
        for (int k = 0; k < frameLen_; ++k) acc += (double) frame_[(size_t) k] * frame_[(size_t) k];
        const float rms = (float) std::sqrt (acc / (double) frameLen_);
        const float rmsDb = rms > 1.0e-9f ? 20.0f * std::log10 (rms) : -120.0f;
        if (rmsDb < kSilenceGateDb) { onUnvoiced (rmsDb); return; }

        // 1. difference function, tau in 1..tauMax.
        const float* x = frame_.data();
        for (int tau = 1; tau <= tauMax_; ++tau)
        {
            const float* a = x;
            const float* b = x + tau;
            float sum = 0.0f;
            for (int j = 0; j < W_; ++j)
            {
                const float d = a[j] - b[j];
                sum += d * d;
            }
            diff_[(size_t) tau] = sum;
        }

        // 2. cumulative mean normalised difference.
        cmndf_[0] = 1.0f;
        double runSum = 0.0;
        for (int tau = 1; tau <= tauMax_; ++tau)
        {
            runSum += (double) diff_[(size_t) tau];
            cmndf_[(size_t) tau] = runSum > 1.0e-12
                                 ? (float) ((double) diff_[(size_t) tau] * tau / runSum)
                                 : 1.0f;
        }

        // 3. absolute threshold: first dip under kYinThreshold, descended to
        //    its local minimum; global minimum as the fallback.
        int rawTau = -1;
        for (int tau = tauMin_; tau <= tauMax_; ++tau)
        {
            if (cmndf_[(size_t) tau] < kYinThreshold)
            {
                while (tau + 1 <= tauMax_ && cmndf_[(size_t) (tau + 1)] < cmndf_[(size_t) tau])
                    ++tau;
                rawTau = tau;
                break;
            }
        }
        if (rawTau < 0)
        {
            float best = 1.0e9f;
            for (int tau = tauMin_; tau <= tauMax_; ++tau)
                if (cmndf_[(size_t) tau] < best) { best = cmndf_[(size_t) tau]; rawTau = tau; }
        }

        // TWO SEPARATE THRESHOLDS, and keeping them separate matters.
        //
        //   kMaxAperiodicity is structural: below this nothing is periodic
        //   enough to be worth analysing at all. It never moves.
        //
        //   apLimit is the `tracking` gate - the taste decision of where to
        //   stop trusting a frame, made dialable because it is a taste
        //   decision rather than a fact. Measured on real vocals
        //   (PITCH_P0_VALIDATION.md §5), residual >600 cent jumps concentrate
        //   in the narrow confidence band immediately above it: 49.5% sit in
        //   0.60-0.70 against 8.1% of voiced frames.
        //
        // The gate decides WHETHER TO PUBLISH, never WHICH CANDIDATE WINS, so
        // it is applied at the END. Wiring it to the entry check instead makes
        // a tighter setting quietly DISABLE guard corrections - the guard's
        // pick gets rejected for being less periodic than the gate allows and
        // the known-wrong rawTau is published in its place, which is worse at
        // every setting. Measured: that wiring gave 1.96/3.06 residual per
        // 1000 where this one gives the numbers in §6.
        const float apLimit = 1.0f - trackingConfidence (
            trackingReq_.load (std::memory_order_relaxed));

        if (cmndf_[(size_t) rawTau] > kMaxAperiodicity) { onUnvoiced (rmsDb); return; }

        // 4. the harmonic guard: re-score tau against a LATTICE of harmonically
        //    related lags, each descended to its own local minimum, with a
        //    continuity handicap toward the recent f0.
        //
        //    WHY A LATTICE AND NOT JUST OCTAVES. Measured on real singing
        //    (PITCH_P0_VALIDATION.md §3.1), third-harmonic confusions are as
        //    frequent as octave ones: grouping every >600 cent single-frame
        //    jump on a clean solo take at its correct voice_type gave 21 at x2
        //    and 24 at x3. A guard whose candidates are only {tau/2, tau*2} is
        //    structurally blind to all of them.
        //
        //    WHY THE DIRECTIONS ARE TREATED DIFFERENTLY. A LARGER tau reports a
        //    LOWER f0. Any signal periodic at T is also periodic at 2T and 3T,
        //    so every sub-multiple lag TIES on aperiodicity — which means a
        //    downward claim must show real CMNDF evidence of a present-but-weak
        //    fundamental (the classic doubling trap) or continuity would lock
        //    every clean tone onto a sub-harmonic after any transient. An
        //    upward claim has no such tie (the shorter lag of a true period
        //    reads strongly aperiodic), so continuity alone may decide it, and
        //    that is what recovers a harmonic lock. Applying one rule to both
        //    directions would either reintroduce the sub-harmonic lock or leave
        //    the harmonic lock uncaught.
        //
        //    Measured direction split on the same take: outliers reporting too
        //    HIGH outnumber too-low ones 324 to 160, so the correction that
        //    matters most moves the estimate DOWN — the evidence-gated
        //    direction, which is the safe one to widen.
        const float fRef    = continuityRef();
        const float costRaw = candidateCost (rawTau, fRef);
        int   chosen     = rawTau;
        float chosenCost = costRaw;
        bool  guardFired = false;

        for (const auto& cand : kGuardLattice)
        {
            // Rounded tau * num / den.
            const long nominal = ((long) rawTau * cand.num + cand.den / 2) / cand.den;
            if (nominal < tauMin_ || nominal > tauMax_) continue;

            const bool movesDown = cand.num > cand.den;   // larger tau = lower f0
            const int  t = descendToLocalMin ((int) nominal);

            // The descent must not carry the candidate across the lag it is
            // being compared against, or the direction rule stops applying to
            // the claim actually being made.
            if (movesDown ? (t <= rawTau) : (t >= rawTau)) continue;

            const float cost = candidateCost (t, fRef);

            if (movesDown)
            {
                if (! (cmndf_[(size_t) t] + kSubMultipleEvidence < cmndf_[(size_t) rawTau]))
                    continue;
                if (! (cost < costRaw)) continue;
            }
            else
            {
                if (! (cost + kUpClaimMargin < costRaw)) continue;
            }

            if (cost < chosenCost) { chosenCost = cost; chosen = t; guardFired = true; }
        }

        // A guard choice still has to look periodic on its own.
        if (cmndf_[(size_t) chosen] > kMaxAperiodicity) { chosen = rawTau; guardFired = false; }

        // The tracking gate, applied to what we actually believe. If the
        // guard has decided the true period is a sub-multiple whose
        // aperiodicity we do not trust, the honest answer is "untracked" -
        // NOT the raw answer we already believe to be wrong.
        if (cmndf_[(size_t) chosen] > apLimit) { onUnvoiced (rmsDb); return; }

        // 5. parabolic interpolation of d' around the chosen lag.
        const float tauF = refineTau (chosen);
        float f0 = (float) (fsA_ / (double) tauF);

        // The lag grid is integer, so tauMin/tauMax straddle the advertised
        // range by a fraction of a bin (a few cents). Clamp the published
        // value so the range the ParamSchema advertises is EXACTLY the range
        // this device can report — the contract holds to the number, not to
        // within a rounding error.
        {
            const VoiceRange& vr = voiceRange (curType_);
            f0 = std::clamp (f0, vr.fMinHz, vr.fMaxHz);
        }

        // 6. short median over recent estimates (spec §2.1). Median-of-3
        //    swallows a single-hop octave spike the candidate pass missed.
        hist_[histCount_ % kHistLen] = f0;
        ++histCount_;
        const float published = medianOf3();
        if (std::fabs (centsBetween (published, f0)) > 600.0f) guardFired = true;

        if (guardFired) guardFires_.fetch_add (1, std::memory_order_relaxed);
        voicedHops_.fetch_add (1, std::memory_order_relaxed);
        hopsSinceVoiced_ = 0;
        lastF0_ = published;

        const float ap = cmndf_[(size_t) chosen];
        outF0_.store (published, std::memory_order_relaxed);
        outAp_.store (ap, std::memory_order_relaxed);
        outConf_.store (std::clamp (1.0f - ap, 0.0f, 1.0f), std::memory_order_relaxed);
        outRmsDb_.store (rmsDb, std::memory_order_relaxed);
        outVoiced_.store (true, std::memory_order_relaxed);
    }

    void onUnvoiced (float rmsDb) noexcept
    {
        if (hopsSinceVoiced_ < (1 << 24)) ++hopsSinceVoiced_;
        if (hopsSinceVoiced_ > historyClearHops_) histCount_ = 0;
        publishUnvoiced (rmsDb);
    }

    void publishUnvoiced (float rmsDb) noexcept
    {
        outVoiced_.store (false, std::memory_order_relaxed);
        outF0_.store (0.0f, std::memory_order_relaxed);
        outConf_.store (0.0f, std::memory_order_relaxed);
        outAp_.store (1.0f, std::memory_order_relaxed);
        outRmsDb_.store (rmsDb, std::memory_order_relaxed);
    }

    // Cost of claiming lag t: its aperiodicity plus a handicap per octave of
    // distance from the recent f0 — the "continuity bias toward the previous
    // f0" of spec §2.1. No recent f0 (fRef 0) = plain aperiodicity.
    float candidateCost (int t, float fRef) const noexcept
    {
        float cost = cmndf_[(size_t) t];
        if (fRef > 0.0f)
            cost += kOctaveBiasPerOct * std::fabs (std::log2 ((float) (fsA_ / (double) t) / fRef));
        return cost;
    }

    float continuityRef() const noexcept
    {
        if (hopsSinceVoiced_ <= continuityMaxHops_ && lastF0_ > 0.0f) return lastF0_;
        if (histCount_ >= 3) return medianOf3();
        return 0.0f;
    }

    int descendToLocalMin (int t) const noexcept
    {
        int budget = std::max (4, t / 8);      // the true dip sits within a few
        while (budget-- > 0)                   // samples of the doubled/halved lag
        {
            if (t - 1 >= tauMin_ && cmndf_[(size_t) (t - 1)] < cmndf_[(size_t) t]) { --t; continue; }
            if (t + 1 <= tauMax_ && cmndf_[(size_t) (t + 1)] < cmndf_[(size_t) t]) { ++t; continue; }
            break;
        }
        return t;
    }

    float refineTau (int t) const noexcept
    {
        if (t <= 1 || t >= tauMax_) return (float) t;
        const float a = cmndf_[(size_t) (t - 1)];
        const float b = cmndf_[(size_t) t];
        const float c = cmndf_[(size_t) (t + 1)];
        const float denom = a - 2.0f * b + c;
        if (std::fabs (denom) < 1.0e-12f) return (float) t;
        const float delta = std::clamp (0.5f * (a - c) / denom, -1.0f, 1.0f);

        // Clamp back INTO the search range. Without this, a lag pinned at
        // tauMin can be refined BELOW it and report an f0 above the voice
        // type's advertised ceiling — measured at 99 hops (0.24%) reaching
        // 521 Hz on a low_male run whose ceiling is 500. That is a dialability
        // bug as much as a DSP one: voice_type's range is a number the model
        // reasons against, and a device that reports outside its own
        // advertised range breaks the contract the whole suite runs on.
        return std::clamp ((float) t + delta, (float) tauMin_, (float) tauMax_);
    }

    float medianOf3() const noexcept
    {
        const int n = histCount_ < kHistLen ? (int) histCount_ : kHistLen;
        if (n <= 0) return 0.0f;
        if (n == 1) return hist_[(histCount_ - 1) % kHistLen];
        if (n == 2)
        {
            const float a = hist_[(histCount_ - 1) % kHistLen];
            const float b = hist_[(histCount_ - 2) % kHistLen];
            return 0.5f * (a + b);
        }
        const float a = hist_[(histCount_ - 1) % kHistLen];
        const float b = hist_[(histCount_ - 2) % kHistLen];
        const float c = hist_[(histCount_ - 3) % kHistLen];
        return std::max (std::min (a, b), std::min (std::max (a, b), c));
    }

    // ---- anti-alias biquad (RBJ lowpass) ----------------------------------
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;

        inline float process (float x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float) y;
        }

        void makeLowpass (double fs, double fc, double q)
        {
            const double w0 = 2.0 * 3.14159265358979323846 * fc / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * q);
            const double a0 = 1.0 + alpha;
            b0 = ((1.0 - cw) * 0.5) / a0;
            b1 =  (1.0 - cw)        / a0;
            b2 = ((1.0 - cw) * 0.5) / a0;
            a1 = (-2.0 * cw)        / a0;
            a2 =  (1.0 - alpha)     / a0;
        }
        void resetState() { z1 = z2 = 0; }
    };

    // ---- audio-thread state -----------------------------------------------
    double fs_  = 48000.0;
    double fsA_ = 48000.0;
    int    curType_ = -1;
    int    decim_ = 1, decimPhase_ = 0;
    int    tauMin_ = 2, tauMax_ = 3, W_ = 0, frameLen_ = 0;
    int    hopA_ = 128, sinceHop_ = 0;
    Biquad aa1_, aa2_;

    std::vector<float> ring_;
    uint32_t ringMask_ = 0;
    int      ringBits_ = 0;
    uint64_t wr_ = 0;

    std::vector<float> frame_, diff_, cmndf_;

    static constexpr int kHistLen = 5;
    float    hist_[kHistLen] = {};
    uint32_t histCount_ = 0;
    float    lastF0_ = 0.0f;
    int      hopsSinceVoiced_ = 1 << 24;
    int      continuityMaxHops_ = 32;
    int      historyClearHops_ = 64;

    // ---- parameters / published results -----------------------------------
    std::atomic<int>   voiceTypeReq_ { kAltoTenor };
    std::atomic<int>   trackingReq_  { kNormal };

    std::atomic<bool>  outVoiced_ { false };
    std::atomic<float> outF0_     { 0.0f };
    std::atomic<float> outConf_   { 0.0f };
    std::atomic<float> outAp_     { 1.0f };
    std::atomic<float> outRmsDb_  { -120.0f };

    std::atomic<uint32_t> guardFires_ { 0 };
    std::atomic<uint32_t> voicedHops_ { 0 };
    std::atomic<uint32_t> totalHops_  { 0 };
};

} // namespace echojay
