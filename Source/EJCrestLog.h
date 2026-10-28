#pragma once
#include <JuceHeader.h>
#include "MeterEngine.h"
#include <algorithm>
#include <cmath>
#include <vector>

/** ---- crestLog: THE TWO CREST MEASURES, LOGGED AND NEVER SHOWN (C4 rev 4 §6; C2 §4) ---------

    Sean's ruling of 28 Oct, after K's MA10 §10 and §11: **crest decides nothing before the beta and
    reaches no user.** Her confirmation test is why - the gap between a real drum excess and a body
    excess is 0.13 dB, because in a finished mix the band is already mostly the peaky material. So
    `bandCrest` is null on the wire, no crest finding is ever written, and **no band move is withheld
    for crest**.

    What is still computed, for tuning, is here. Two measures per band per side, plus what they were
    measured under:

      V0  10*log10( 95th percentile over qualifying hops of ( bandPower_h / its 3 s local mean ) )
      V1  the median over qualifying hops of 10*log10( bandPeak_h / bandPower_h )   ("as available")

    NOTHING RENDERS THIS. No layout reads it and `EJMatchReport.h` does not take it; it rides the
    `match_report` payload's record and goes to the server with the Match request. A field nobody
    draws is the point.

    AND IT CARRIES WHAT IT WAS MEASURED UNDER. Before the parked 256-sample branch merges, V1 reads
    `bandPeak` from ONE FFT PER HOST BLOCK, so it carries a block-size bias (up to 0.38 dB on her
    stems). `hostBlockSize` and `framesPerHop` travel with the figures so that bias can be corrected
    later - a figure that does not say what it was measured under cannot be checked, which is the
    same rule `macroAccumSeconds` exists for.                                                      */
namespace echojay::crestlog
{

/** C4 rev 4 §6's own constants, named rather than spelled into the arithmetic. */
inline constexpr int    kLocalMeanBefore   = 15;    ///< hops h-15 ...
inline constexpr int    kLocalMeanAfter    = 14;    ///< ... to h+14: thirty hops, three seconds
inline constexpr int    kLocalMeanMinHops  = 10;    ///< at least ten qualifying in that window
inline constexpr int    kMinQualifyingHops = 100;   ///< under this the band is NULL, not a figure
inline constexpr double kV0Percentile      = 0.95;

/** One band's pair. `has*` is the null C4 asks for: absent is not 0 dB, and 0 dB of crest is a real
    reading (a band whose peak is its mean). */
struct BandPair
{
    bool   hasV0 = false; double v0Db = 0.0;
    bool   hasV1 = false; double v1Db = 0.0;
    int    qualifyingHops = 0;
};

struct SideLog
{
    std::array<BandPair, 6> bands {};
    double framesPerHop = 0.0;    ///< the MEAN of bandFrames over qualifying hops: what V1's max was over
};

/** A hop qualifies when it has spectral evidence and was not all-silent - C4 rev 4 §6: "bandFrames
    > 0 and flags bit 0 clear". Written once because three loops below ask it. */
inline bool qualifies (const MeterEngine::HopFrame& h) noexcept
{
    return h.bandFrames > 0 && (h.flags & 1u) == 0u;
}

inline double percentileOf (std::vector<double> xs, double p)
{
    if (xs.empty()) return 0.0;
    std::sort (xs.begin(), xs.end());
    const double pos = p * (double) (xs.size() - 1);
    const size_t i = (size_t) std::floor (pos);
    if (i + 1 >= xs.size()) return xs.back();
    const double frac = pos - (double) i;
    return xs[i] * (1.0 - frac) + xs[i + 1] * frac;
}
inline double medianOf (std::vector<double> xs)
{
    if (xs.empty()) return 0.0;
    std::sort (xs.begin(), xs.end());
    const size_t n = xs.size();
    return (n % 2 == 1) ? xs[n / 2] : 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
}

/** One side's log, over the hops of the SELECTED WINDOW - the caller slices; this measures what it
    is given, and says how many hops qualified so a short window is visible rather than implied. */
inline SideLog sideLogFrom (const std::vector<MeterEngine::HopFrame>& hops)
{
    SideLog out;
    // The qualifying indices once, because every band walks the same set and "qualifying" must not
    // mean something different per band.
    std::vector<size_t> q;
    q.reserve (hops.size());
    double framesSum = 0.0;
    for (size_t i = 0; i < hops.size(); ++i)
        if (qualifies (hops[i])) { q.push_back (i); framesSum += (double) hops[i].bandFrames; }
    out.framesPerHop = q.empty() ? 0.0 : framesSum / (double) q.size();

    for (int bi = 0; bi < 6; ++bi)
    {
        auto& b = out.bands[(size_t) bi];
        b.qualifyingHops = (int) q.size();
        if ((int) q.size() < kMinQualifyingHops) continue;   // NULL, per §6, and not a 0

        // ---- V0: the band's own power against its 3 s local mean ---------------------------
        std::vector<double> ratios;
        ratios.reserve (q.size());
        for (const size_t h : q)
        {
            const double here = (double) hops[h].bandPower[(size_t) bi];
            if (here <= 0.0) continue;
            double sum = 0.0; int n = 0;
            const long lo = (long) h - kLocalMeanBefore, hi = (long) h + kLocalMeanAfter;
            for (long k = std::max (0L, lo); k <= hi && k < (long) hops.size(); ++k)
            {
                const auto& hf = hops[(size_t) k];
                if (! qualifies (hf)) continue;              // the same predicate, not a second one
                sum += (double) hf.bandPower[(size_t) bi];
                ++n;
            }
            if (n < kLocalMeanMinHops) continue;             // this hop has no local mean to speak of
            const double mean = sum / (double) n;
            if (mean <= 0.0) continue;
            ratios.push_back (here / mean);
        }
        if (! ratios.empty())
        {
            const double pct = percentileOf (ratios, kV0Percentile);
            if (pct > 0.0) { b.hasV0 = true; b.v0Db = 10.0 * std::log10 (pct); }
        }

        // ---- V1 "as available": peak over mean, within the hop ------------------------------
        // It reads bandPeak, which before the 256-sample cadence is a max over one FFT per host
        // block - hence framesPerHop and the caller's hostBlockSize travelling with it.
        std::vector<double> v1;
        v1.reserve (q.size());
        for (const size_t h : q)
        {
            const double pw = (double) hops[h].bandPower[(size_t) bi];
            const double pk = (double) hops[h].bandPeak [(size_t) bi];
            if (pw <= 0.0 || pk <= 0.0) continue;
            v1.push_back (10.0 * std::log10 (pk / pw));
        }
        if (! v1.empty()) { b.hasV1 = true; b.v1Db = medianOf (v1); }
    }
    return out;
}

/** The payload shape C2 §4 specifies. A band with no figure is `null`, never 0 - the rule every
    figure in that payload follows. */
inline juce::var sideVar (const SideLog& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("framesPerHop", juce::String (s.framesPerHop, 2).getDoubleValue());
    juce::Array<juce::var> v0, v1;
    for (int bi = 0; bi < 6; ++bi)
    {
        const auto& b = s.bands[(size_t) bi];
        v0.add (b.hasV0 ? juce::var (juce::String (b.v0Db, 3).getDoubleValue()) : juce::var());
        v1.add (b.hasV1 ? juce::var (juce::String (b.v1Db, 3).getDoubleValue()) : juce::var());
    }
    o->setProperty ("v0", juce::var (v0));
    o->setProperty ("v1", juce::var (v1));
    o->setProperty ("qualifyingHops", s.bands[0].qualifyingHops);
    return juce::var (o);
}

/** ---- B'S BOUNDS, CHECKED BEFORE SENDING AND NEVER CLAMPED (28 Oct 2026) -------------------

    The server takes this shape exactly and drops an OUT-OF-BOUNDS LOG - never the block - so a
    figure outside the bounds costs the log and nothing else: `hostBlockSize` an integer 16..8192,
    `framesPerHop` 0..1000, each v entry -10..40 dB or null, v0 required and v1 as available.

    CHECKED, NOT CLAMPED. A clamped figure is a wrong number that passes, which is worse than a
    missing log - and the whole point of crestLog is tuning, so a quietly corrected value would tune
    against itself. The caller asks this, and when it answers false it says so in the log and sends
    no crestLog at all. */
inline bool withinServerBounds (const SideLog& s, int hostBlockSize, juce::String* whyOut = nullptr)
{
    const auto no = [whyOut] (const juce::String& why) { if (whyOut != nullptr) *whyOut = why; return false; };
    if (hostBlockSize < 16 || hostBlockSize > 8192)
        return no ("hostBlockSize " + juce::String (hostBlockSize) + " is outside 16..8192");
    if (s.framesPerHop < 0.0 || s.framesPerHop > 1000.0)
        return no ("framesPerHop " + juce::String (s.framesPerHop, 2) + " is outside 0..1000");
    for (int bi = 0; bi < 6; ++bi)
    {
        const auto& b = s.bands[(size_t) bi];
        // v0 is REQUIRED where there are figures at all: a side with enough qualifying hops that
        // produced no v0 is a fault in this file, not a null the server should take.
        if (b.qualifyingHops >= kMinQualifyingHops && ! b.hasV0)
            return no ("band " + juce::String (bi) + " has " + juce::String (b.qualifyingHops)
                       + " qualifying hops and no v0");
        if (b.hasV0 && (b.v0Db < -10.0 || b.v0Db > 40.0))
            return no ("band " + juce::String (bi) + "'s v0 " + juce::String (b.v0Db, 2)
                       + " dB is outside -10..40");
        if (b.hasV1 && (b.v1Db < -10.0 || b.v1Db > 40.0))
            return no ("band " + juce::String (bi) + "'s v1 " + juce::String (b.v1Db, 2)
                       + " dB is outside -10..40");
    }
    return true;
}

inline juce::var crestLogVar (const SideLog& reference, const SideLog& mix, int hostBlockSize)
{
    auto* sides = new juce::DynamicObject();
    sides->setProperty ("reference", sideVar (reference));
    sides->setProperty ("mix",       sideVar (mix));
    auto* o = new juce::DynamicObject();
    o->setProperty ("v", 1);
    o->setProperty ("hostBlockSize", hostBlockSize);
    o->setProperty ("sides", juce::var (sides));
    return juce::var (o);
}

} // namespace echojay::crestlog
