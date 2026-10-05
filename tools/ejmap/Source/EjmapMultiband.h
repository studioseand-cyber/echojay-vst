/*
  EjmapMultiband.h - MULTIBAND COMPRESSORS (docs/MULTIBAND_PROFILE_PROPOSAL.md). PROTOTYPE, 5 Oct 2026 (overnight run 2,
  R7). No spec change: this is the proposal's method with real numbers, nothing exported.

  PURE:
  - THE BANDS: from the crossover controls' instantiate displays (Hz, ascending) the band edges 20 .. x1 .. xn .. 20000 and
    each band's geometric centre - the tone each band is swept with. Crossovers that print "Off" or no number are skipped
    and said; a unit with no crossover controls gets no centres (nothing is assumed).
  - THE BAND LADDERS: the probe's --sweep at the band's centre, that band's threshold at N norms, five levels; GR against the
    open end (EjmapDeesser's ladderOf, the same reading).
  - THE WHOLE-UNIT FIGURE: the probe's --response with shape=vocal (a speech-like multitone) at five levels, the unit at its
    instantiate state (open) and at the offset / global position; gain = total output power over total input power across
    the tones; GR = open gain - position gain, per level.
  - THE AMOUNT, two kinds (proposal item 3): a GLOBAL control where one exists (walked as the amount), else a COMMON dB OFFSET
    written to every band threshold: offset -> norm per band from the control's own range (its two numeric ends), the
    instantiate value plus the offset, clamped to the control.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapEq.h"
#include "EjmapDeesser.h"
#include <vector>
#include <optional>
#include <map>

namespace ejmap::multiband
{

struct Band { int index = 0; double loHz = 0.0, hiHz = 0.0, centreHz = 0.0; };
// crossover displays -> bands; a display that is not a number is skipped and named
inline std::vector<Band> bandsFromCrossovers (const std::vector<juce::String>& displays, juce::StringArray& skipped)
{
    std::vector<double> edges;
    for (const auto& d : displays) { if (const auto hz = deesser::labelHz (d); hz && *hz > 20.0 && *hz < 20000.0) edges.push_back (*hz); else skipped.add (d); }
    std::sort (edges.begin(), edges.end());
    std::vector<Band> out; double lo = 20.0; int k = 1;
    for (double e : edges) { if (e <= lo) continue; out.push_back ({ k++, lo, e, std::sqrt (lo * e) }); lo = e; }
    out.push_back ({ k, lo, 20000.0, std::sqrt (lo * 20000.0) });
    return out;
}

// a threshold control's dB ends from its displays at norm 0 and 1 (both numeric), and the norm for a dB value (linear in dB, said)
struct DbRange { bool ok = false; double at0 = 0.0, at1 = 0.0; };
inline DbRange dbRangeOf (const juce::String& display0, const juce::String& display1)
{
    DbRange r; auto num = [] (const juce::String& t) -> std::optional<double> { const auto s = t.trim(); juce::String n; for (int i = 0; i < s.length(); ++i) { const auto c = s[i]; if (juce::CharacterFunctions::isDigit (c) || c == '.' || c == '-' || c == '+') n << c; else if (n.isNotEmpty()) break; } if (n.isEmpty() || n == "-" || n == "+" || n == ".") return {}; return n.getDoubleValue(); };
    const auto a = num (display0), b = num (display1);
    if (! a || ! b || *a == *b) return r;
    r.ok = true; r.at0 = *a; r.at1 = *b; return r;
}
inline float normForDb (const DbRange& r, double db) { return (float) juce::jlimit (0.0, 1.0, (db - r.at0) / (r.at1 - r.at0)); }

// the whole-unit gain from a vocal-shaped response position: total output power over total input power across the tones
inline std::optional<double> totalGainDb (const eq::Position& p)
{
    double in = 0.0, out = 0.0; int n = 0;
    for (const auto& t : p.tones) { if (t.inDb < -200.0 || t.outDb < -200.0) continue; in += std::pow (10.0, t.inDb / 10.0); out += std::pow (10.0, t.outDb / 10.0); ++n; }
    if (n == 0 || in <= 0.0) return {};
    return 10.0 * std::log10 (out / in);
}

struct OffsetPoint { double offsetDb = 0.0; juce::String displays; std::map<double, double> grByLevel; };   // level dBFS -> whole-unit GR

} // namespace ejmap::multiband
