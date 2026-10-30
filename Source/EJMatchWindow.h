#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include "EJMatchProposal.h"   // matchTooShortMessage, for the loudest-30 refusal

// =============================================================================
//  MATCH, THE HOP-SERIES FIGURES: six-band crest, PSR and the loudest-30 window.
// =============================================================================
//
// C4 (CONTRACT_MATCH_ATTRIBUTION_2026-10-27, revision 4) sections 6 and 7, read
// off C2 section 5's hop series: one frame per sample-counted 100 ms hop, kept
// for the whole capture and the whole reference.
//
// A TEMPLATE OVER THE FRAME, ON PURPOSE. The frame type is A's (C2 section 5)
// and lands after k-base-1. Every function here reads exactly C2's field names
// (shortTermLufs, shortTermTruePeak, bandPower, bandPeak, bandFrames, flags), so
// that type drops in with no adapter, and the gate drives the same code with a
// local struct carrying the same fields. Nothing here names that type.
//
// PURE. No JUCE audio type, no global, no I/O. The one JUCE dependency is the
// refusal sentence, which comes from EJMatchProposal.h like every other Match
// refusal.
//
// ABSENT IS NEVER 0. A figure that cannot be computed is std::nullopt. A crest of
// 0 dB is a real measurement (a band that never rises above its own level), and
// it must not be confused with "no crest".
//
// bandPeak IS NEVER READ BY bandCrest. It is a maximum over FFT frames, and the
// number of frames in a hop follows the host's block size (C2 section 6), so the
// same audio reads peakier at 128 samples than at 2048. C4 section 6 rebuilt
// crest on bandPower alone. A crest that reads bandPeak is the defect rev 2
// removed.
//
// CREST DECIDES NOTHING (C4 rev 4 section 6). Before the beta both crest
// figures here are LOGGED for tuning and never shown or routed on: bandCrest
// (V0) and bandCrestPeakOverMean (V1 as available). The second is the one place
// bandPeak is read, and it carries the block-size bias C2 section 6 names, which
// is why meanFramesPerHop is logged beside it.
// =============================================================================

namespace echojay
{

// -----------------------------------------------------------------------------
// THE CONSTANTS
// -----------------------------------------------------------------------------

/** C4 section 6 step 2: the local level runs from hop h - 15 to hop h + 14,
    which is 30 hops, 3 s. */
inline constexpr int kCrestLocalHopsBefore = 15;
inline constexpr int kCrestLocalHopsAfter  = 14;

/** C4 section 6 step 2: skip hop h when fewer than this many hops of its local
    span qualify. */
inline constexpr int kCrestLocalMinHops = 10;

/** C4 section 6 step 5, PROPOSED: fewer qualifying hops than this (10 s) gives
    no crest. */
inline constexpr int kCrestMinQualifyingHops = 100;

/** C4 section 6 step 4: the percentile the crest is read at. */
inline constexpr int kCrestPercentile = 95;

/** C2 section 5 flags bit 0: every block in this hop was silent. */
inline constexpr std::uint16_t kHopFlagSilent = 1;

/** C4 section 7: a hop is usable for loudness from this index on. The first 29
    hops have no full 3 s short-term window, and C2 says they read -100. */
inline constexpr std::size_t kUsableHopFirstIndex = 29;

/** C4 section 7: and only when its short-term loudness is above this, in LUFS. */
inline constexpr float kUsableHopMinLufs = -70.0f;

/** C4 section 7: the loudest-30 window is 300 consecutive hops. */
inline constexpr std::size_t kLoudest30Hops = 300;

/** C2 section 5: the index is the clock, and one hop is 100 ms. */
inline constexpr double kHopSeconds = 0.1;

/** NOT C4's. Two loudest-30 means within this relative difference are a tie
    (see loudest30Window). Far below anything two different windows of audio
    measure, and far above double rounding over 300 terms. */
inline constexpr double kLoudest30TieRelative = 1e-12;

// -----------------------------------------------------------------------------
// SHARED PIECES
// -----------------------------------------------------------------------------

/** A span of hops, [begin, end). "whole" is { 0, frames.size() }. */
struct HopRange
{
    std::size_t begin = 0;
    std::size_t end   = 0;
};

/** C4 section 6 step 1: a hop qualifies when it holds spectral evidence and is
    not silent. bandFrames == 0 is an absence, never a level of zero. */
template <typename Frame>
inline bool crestHopQualifies (const Frame& f) noexcept
{
    return f.bandFrames > 0 && (f.flags & kHopFlagSilent) == 0;
}

/** C4 section 7: usable for loudness. */
template <typename Frame>
inline bool loudnessHopUsable (const Frame& f, std::size_t index) noexcept
{
    return index >= kUsableHopFirstIndex && f.shortTermLufs > kUsableHopMinLufs;
}

/** The 95th percentile BY NEAREST RANK: the value at index ceil(0.95 * n) - 1 of
    the n values sorted ascending. Nearest rank, not interpolation, so the
    figure is always one of the measured ratios and the result is the same on
    every machine.

    INTEGER ARITHMETIC FOR THE CEILING. 0.95 is not exact in binary, so
    ceil(0.95 * n) in floating point can land one index off for some n. It is
    computed as (95 * n + 99) / 100, which is the same ceiling with no rounding.
    n must be at least 1. */
inline std::size_t nearestRankIndex (std::size_t n, int percentile) noexcept
{
    const std::size_t p = (std::size_t) percentile;
    return (p * n + 99) / 100 - 1;
}

// -----------------------------------------------------------------------------
// SIX-BAND CREST (C4 section 6)
// -----------------------------------------------------------------------------

struct BandCrest
{
    /** In dB, per band, or nullopt when the band has too little data. */
    std::array<std::optional<float>, 6> db {};
    /** Hops that passed step 1 in the window. The same for every band, because
        bandFrames and the silent flag belong to the hop, not to a band. */
    int qualifyingHops = 0;
    /** Per band, how many hops gave a ratio: qualifying, with at least
        kCrestLocalMinHops qualifying neighbours, and a local level above 0. */
    std::array<int, 6> ratioHops {};
};

/** One side's band crest over a window of its hop series.

    The steps are C4 section 6's, in order:
      1. qualifying hops: bandFrames > 0 and flags bit 0 clear;
      2. M_h[b], the mean bandPower[b] over the qualifying hops from h - 15 to
         h + 14; h is skipped when fewer than 10 of them qualify;
      3. r_h[b] = bandPower_h[b] / M_h[b];
      4. crest[b] = 10 * log10 (95th percentile of r_h[b]), by nearest rank;
      5. fewer than 100 qualifying hops gives nullopt.

    TWO READINGS C4 DOES NOT FIX, stated here so they are visible:
      - The local span stops at the window's edges. In "whole" that changes
        nothing. In a loudest-30 window, M_h near an edge is taken over fewer
        hops instead of reaching outside the window, so the figure describes only
        the audio it names.
      - A local level of 0 gives no ratio for that hop and band, instead of a
        division by zero. A band with no ratio at all is nullopt. */
template <typename Frame>
inline BandCrest bandCrest (const std::vector<Frame>& frames, HopRange w)
{
    BandCrest out;
    w.end   = std::min (w.end, frames.size());
    w.begin = std::min (w.begin, w.end);
    const std::size_t n = w.end - w.begin;
    if (n == 0) return out;

    // Prefix sums over the window, so each local mean is O(1). Double, because
    // the sums run over thousands of hops of float powers.
    std::vector<int> qCount (n + 1, 0);
    std::array<std::vector<double>, 6> pSum;
    for (auto& v : pSum) v.assign (n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& f = frames[w.begin + i];
        const bool q = crestHopQualifies (f);
        qCount[i + 1] = qCount[i] + (q ? 1 : 0);
        for (std::size_t b = 0; b < 6; ++b)
            pSum[b][i + 1] = pSum[b][i] + (q ? (double) f.bandPower[b] : 0.0);
    }
    out.qualifyingHops = qCount[n];

    std::array<std::vector<double>, 6> ratios;
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& f = frames[w.begin + i];
        if (! crestHopQualifies (f)) continue;
        const std::size_t lo = i >= (std::size_t) kCrestLocalHopsBefore ? i - (std::size_t) kCrestLocalHopsBefore : 0;
        const std::size_t hi = std::min (n - 1, i + (std::size_t) kCrestLocalHopsAfter);
        const int q = qCount[hi + 1] - qCount[lo];
        if (q < kCrestLocalMinHops) continue;
        for (std::size_t b = 0; b < 6; ++b)
        {
            const double m = (pSum[b][hi + 1] - pSum[b][lo]) / (double) q;
            if (! (m > 0.0)) continue;
            ratios[b].push_back ((double) f.bandPower[b] / m);
        }
    }

    for (std::size_t b = 0; b < 6; ++b)
    {
        auto& r = ratios[b];
        out.ratioHops[b] = (int) r.size();
        if (out.qualifyingHops < kCrestMinQualifyingHops || r.empty()) continue;
        std::sort (r.begin(), r.end());
        const double p = r[nearestRankIndex (r.size(), kCrestPercentile)];
        if (! (p > 0.0)) continue;   // 95% of the ratios are 0: no level to read
        out.db[b] = (float) (10.0 * std::log10 (p));
    }
    return out;
}

/** The whole file or capture. */
template <typename Frame>
inline BandCrest bandCrest (const std::vector<Frame>& frames)
{
    return bandCrest (frames, HopRange { 0, frames.size() });
}

// -----------------------------------------------------------------------------
// CREST, LOGGED ONLY (C4 rev 4 section 6): V1 AS AVAILABLE, AND FRAMES PER HOP
// -----------------------------------------------------------------------------

/** V1 as available: per band, the median over qualifying hops of
    10 log10 (bandPeak / bandPower). A hop with no power in the band gives no
    ratio; a band with no ratio is nullopt. An even count takes the mean of the
    two middle values, as psrMedian does. Before A's 256-sample cadence this
    reads bandPeak from one FFT per host block, so it moves with the block
    size. LOGGED, NEVER DECIDING. */
template <typename Frame>
inline std::array<std::optional<float>, 6> bandCrestPeakOverMean (const std::vector<Frame>& frames, HopRange w)
{
    std::array<std::optional<float>, 6> out {};
    const std::size_t end = std::min (w.end, frames.size());
    std::array<std::vector<double>, 6> r;
    for (std::size_t h = w.begin; h < end; ++h)
    {
        const auto& f = frames[h];
        if (! crestHopQualifies (f)) continue;
        for (std::size_t b = 0; b < 6; ++b)
            if (f.bandPower[b] > 0.0f && f.bandPeak[b] > 0.0f)
                r[b].push_back (10.0 * std::log10 ((double) f.bandPeak[b] / (double) f.bandPower[b]));
    }
    for (std::size_t b = 0; b < 6; ++b)
    {
        if (r[b].empty()) continue;
        std::sort (r[b].begin(), r[b].end());
        const std::size_t n = r[b].size(), m = n / 2;
        out[b] = (float) (n % 2 == 1 ? r[b][m] : 0.5 * (r[b][m - 1] + r[b][m]));
    }
    return out;
}

/** The mean bandFrames over the window's hops that hold spectral evidence:
    the frames per hop C4 rev 4 section 6 logs beside the host block size.
    nullopt when no hop in the window has a frame. */
template <typename Frame>
inline std::optional<float> meanFramesPerHop (const std::vector<Frame>& frames, HopRange w)
{
    const std::size_t end = std::min (w.end, frames.size());
    double sum = 0.0;
    int n = 0;
    for (std::size_t h = w.begin; h < end; ++h)
        if (frames[h].bandFrames > 0) { sum += frames[h].bandFrames; ++n; }
    if (n == 0) return std::nullopt;
    return (float) (sum / n);
}

// -----------------------------------------------------------------------------
// PSR (C4 section 7)
// -----------------------------------------------------------------------------

/** Which window a figure is for. C3 and C4 section 7 treat them differently. */
enum class MatchWindowMode { Whole, Loudest30 };

/** The median over the usable, non-silent hops of shortTermTruePeak minus
    shortTermLufs.

    In Loudest30 it is nullopt: C3's rule, because PSR is present only when it is
    whole-file on both sides. With no qualifying hop it is nullopt. With an even
    count the median is the mean of the two middle values. */
template <typename Frame>
inline std::optional<float> psrMedian (const std::vector<Frame>& frames, MatchWindowMode mode)
{
    if (mode != MatchWindowMode::Whole) return std::nullopt;
    std::vector<double> v;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        const auto& f = frames[i];
        if (! loudnessHopUsable (f, i) || (f.flags & kHopFlagSilent) != 0) continue;
        v.push_back ((double) f.shortTermTruePeak - (double) f.shortTermLufs);
    }
    if (v.empty()) return std::nullopt;
    std::sort (v.begin(), v.end());
    const std::size_t m = v.size() / 2;
    return (float) ((v.size() % 2 == 1) ? v[m] : 0.5 * (v[m - 1] + v[m]));
}

// -----------------------------------------------------------------------------
// THE LOUDEST-30 WINDOW (C4 section 7)
// -----------------------------------------------------------------------------

struct Loudest30
{
    /** The 300 hops, or nullopt when the side has none. */
    std::optional<HopRange> window;
    /** The side's length, so the refusal can name it. */
    double sideSeconds = 0.0;
};

/** The 300 consecutive hops with the highest mean of 10^(shortTermLufs / 10)
    over their usable hops. Ties go to the earliest. A side under 30 s has no
    window.

    EACH WINDOW IS SUMMED FRESH, NOT SLID, AND A TIE IS A TIE TO ROUNDING. A
    running sum that adds one hop and drops another drifts in its last bits.
    Even summed fresh, two windows holding the same hops in a different order
    can differ in the last bit. So a later window replaces the best only when
    its mean is larger by more than kLoudest30TieRelative, which keeps "ties go
    to the earliest" true of the audio rather than of the summation order.
    Summing 300 hops per start is about 3.6 M additions for a 20 minute side,
    which is nothing for a message-thread figure computed once. */
template <typename Frame>
inline Loudest30 loudest30Window (const std::vector<Frame>& frames)
{
    Loudest30 out;
    out.sideSeconds = (double) frames.size() * kHopSeconds;
    if (frames.size() < kLoudest30Hops) return out;

    bool   found = false;
    double best  = 0.0;
    for (std::size_t s = 0; s + kLoudest30Hops <= frames.size(); ++s)
    {
        double sum = 0.0;
        int    count = 0;
        for (std::size_t i = s; i < s + kLoudest30Hops; ++i)
        {
            if (! loudnessHopUsable (frames[i], i)) continue;
            sum += std::pow (10.0, (double) frames[i].shortTermLufs / 10.0);
            ++count;
        }
        if (count == 0) continue;
        const double mean = sum / (double) count;
        // Strict and relative, so a tie keeps the earlier start.
        if (! found || mean > best * (1.0 + kLoudest30TieRelative))
        {
            found = true;
            best  = mean;
            out.window = HopRange { s, s + kLoudest30Hops };
        }
    }
    return out;
}

/** The refusal for a side with no loudest-30 window. It names the side's length
    against the 30 s it needs, in the same words as every other Match length
    refusal. */
inline juce::String loudest30RefusalMessage (const juce::String& side, double sideSeconds)
{
    return matchTooShortMessage ("loudest-30 window", side,
                                 (float) ((double) kLoudest30Hops * kHopSeconds),
                                 (float) sideSeconds);
}

} // namespace echojay
