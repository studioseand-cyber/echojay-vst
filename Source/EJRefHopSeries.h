#pragma once
// =============================================================================================
//  EJRefHopSeries.h - A REFERENCE'S HOP SERIES, STORED IN THE LIBRARY INDEX
//  (Kathy's R4-1, open list 162 / pin MA4; Sean chose her shape 1 on 29 Oct 2026)
// =============================================================================================
//  What was wrong, in her words: a reference restored from the library is rebuilt from the index's
//  RefMeasurements with no re-analysis, and `hopFrames` was neither written nor read back. So after
//  a restore - and for a MOVED file, which cannot be re-analysed at all - the reference had no hop
//  series, and the Match page could not take a loudest-30 window on the reference side. It said so
//  rather than falling back silently, which is the right refusal and the reason this exists.
//
//  SEAN'S CHOICE WAS SHAPE 1: the series itself, about 70 KB of JSON per reference, so "every window
//  and figure K adds later reads it with no further schema change". The alternative stored today's
//  figures and made each new one a schema change.
//
//  ---- WHY FLOATS AND NOT THE STRUCT ---------------------------------------------------------
//
//  EJReferenceIndex.h does not include MeterEngine.h and should not start: the index is a file
//  format, and a file format that depends on a DSP header acquires its rebuilds and its cycles. So
//  the index stores a flat float series with the layout below, and the conversion lives here, next
//  to the type it converts.
//
//  ---- WHAT A HOP CARRIES, AND WHY bandFrames IS NOT OPTIONAL --------------------------------
//
//  Sixteen floats per hop. Kathy's estimate was fourteen - the two extra are `bandFrames` and
//  `flags`, and bandFrames in particular MUST travel: its own comment says "0 = NO spectral
//  evidence: unavailable, never a level". A restored hop that dropped it would read as evidence
//  nobody measured, which is the same class of fabrication as a restored overs count of zero
//  (open list 198, and the -1 sentinel three fields above this one in RefMeasurements).
// =============================================================================================

#include <JuceHeader.h>
#include "MeterEngine.h"

namespace echojay
{

/** Floats per hop in the stored series. Bump this ONLY with a new key name: a stride change read
    against an old array silently reinterprets every number. */
inline constexpr int kRefHopFloats = 16;

/** The layout, named once so the two ends cannot disagree:
      [0] shortTermLufs   [1] shortTermTruePeak
      [2..7] bandPower[6] (linear perOct mean)   [8..13] bandPeak[6] (linear perOct max)
      [14] bandFrames     [15] flags */
inline std::vector<float> refHopSeriesToFloats (const std::vector<MeterEngine::HopFrame>& hops)
{
    std::vector<float> out;
    out.reserve (hops.size() * (size_t) kRefHopFloats);
    for (const auto& h : hops)
    {
        out.push_back (h.shortTermLufs);
        out.push_back (h.shortTermTruePeak);
        for (int b = 0; b < 6; ++b) out.push_back (h.bandPower[(size_t) b]);
        for (int b = 0; b < 6; ++b) out.push_back (h.bandPeak[(size_t) b]);
        out.push_back ((float) h.bandFrames);
        out.push_back ((float) h.flags);
    }
    return out;
}

/** ...and back. A series whose length is not a whole number of hops is REFUSED ENTIRELY rather
    than truncated: half a hop read as a whole one is a wrong figure, and a reference with no series
    says so (the page refuses the window) while a reference with a wrong one does not. */
inline std::vector<MeterEngine::HopFrame> refHopSeriesFromFloats (const std::vector<float>& f)
{
    std::vector<MeterEngine::HopFrame> hops;
    if (f.empty() || (f.size() % (size_t) kRefHopFloats) != 0) return hops;
    hops.reserve (f.size() / (size_t) kRefHopFloats);
    for (size_t i = 0; i + (size_t) kRefHopFloats <= f.size(); i += (size_t) kRefHopFloats)
    {
        MeterEngine::HopFrame h;
        h.shortTermLufs     = f[i];
        h.shortTermTruePeak = f[i + 1];
        for (int b = 0; b < 6; ++b) h.bandPower[(size_t) b] = f[i + 2 + (size_t) b];
        for (int b = 0; b < 6; ++b) h.bandPeak [(size_t) b] = f[i + 8 + (size_t) b];
        h.bandFrames = (std::uint16_t) juce::jlimit (0.0f, 65535.0f, f[i + 14]);
        h.flags      = (std::uint16_t) juce::jlimit (0.0f, 65535.0f, f[i + 15]);
        hops.push_back (h);
    }
    return hops;
}

} // namespace echojay
