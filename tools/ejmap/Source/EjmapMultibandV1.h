/*
  EjmapMultibandV1.h - v1 CERTIFICATION OF MULTIBAND COMPRESSORS, PER BAND (Kathy, 16 Oct 2026, "MULTIBAND BUILD"). Pure: it judges
  the Phase B multiband draft (ej_multiband_profile/0: the per-band ladders, Sean's cert folders) and the v1 measurement record
  (ej_multiband_v1/1, EjmapMultibandV1Run.h), and certifies BAND BY BAND - only a band that passes is offered (as EQ v1).

  A BAND PASSES when every one of these holds:
    identity      a threshold control the MEASUREMENT paired with the band (the pairing response cut that band)
    range         its gain reduction reaches the largest ask, kMaxAskDb (4 dB), on the band's centre tone
    direction     gain reduction moves one way with the threshold (either way; 0.5 dB of reading noise allowed)
    threshold     where compression starts on the band's tone (GR reaches 1 dB, the tone's RMS level) TRACKS the threshold's dB display
                  1:1 - every position within kThresholdTolDb (3 dB) of a common reference offset (0 = dBFS; a dBu-style scale is said);
                  one position alone within 3 dB outright; a display that is not dB, or no onset inside the levels, is "not tested"
    gain          THE PER-BAND TONE CHECK (new measuring): the setting predicted from the ladder for g = 2, 3 and 4 dB at -12 dBFS, the
                  band's centre tone, measured GR within kGainTolDb (0.5 dB) of the ask at each
    crossover     THE CROSSOVERS (new measuring): at the position the plugin opens with (v1 never moves a crossover), the frequency where
                  the two neighbouring bands' responses are equal (each soloed, or each compressed alone with the others open; no crossover
                  type assumed - crossingHz) within 1/6 octave of the crossover's Hz display; a unit with no
                  crossover control (OTT) is "not applicable". v1 leaves crossovers where the plugin opens: the check says the bands are
                  where their displays say
  OFFERED BESIDE A PASSING BAND: its make-up / gain control when the measured level step matches the dB display step within 0.5 dB.
  Units of the draft and the record are matched by plugin_id.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <map>
#include <optional>
#include <cmath>
#include <algorithm>

namespace ejmap::mbv1
{

inline constexpr double kMaxAskDb = 4.0, kThresholdTolDb = 3.0, kGainTolDb = 0.5, kCrossoverOct = 1.0 / 6.0, kMakeupTolDb = 0.5;
inline constexpr double kSineRmsDb = -3.0103;   // a sine's RMS below its peak: the ladder's levels are peak dBFS

struct Part { juce::String name, state, why; };   // pass | fail | not_tested | not_applicable
struct BandResult { int band = 0; juce::String control; double toneHz = 0.0; std::vector<Part> parts; bool pass = false; bool makeupOffered = false; juce::String makeupControl; };
struct Result { juce::String product, pluginId; std::vector<BandResult> bands; bool pass = false;
    juce::StringArray certifiedParts() const { juce::StringArray c; for (const auto& b : bands) if (b.pass) { c.add ("band " + juce::String (b.band)); if (b.makeupOffered) c.add ("band " + juce::String (b.band) + " make-up"); } return c; } };

// a number off a display: "-12.0 dB", "-12", "− 12.0 dB", "1.2 kHz" -> 1200 (Hz only when asked)
inline std::optional<double> numberOf (const juce::String& display, bool hz = false)
{
    auto t = display.trim().replace (juce::String::charToString ((juce::juce_wchar) 0x2212), "-").replace (juce::String::charToString ((juce::juce_wchar) 0x202f), " ");
    if (t.startsWithChar ('+')) t = t.substring (1);
    const auto lead = t.initialSectionContainingOnly ("-0123456789.");
    if (lead.isEmpty() || lead == "-" || lead == ".") return std::nullopt;
    double v = lead.getDoubleValue(); const auto rest = t.substring (lead.length()).trimStart().toLowerCase();
    if (rest.startsWith ("inf")) return std::nullopt;
    if (hz && rest.startsWith ("k")) v *= 1000.0;
    return v;
}

struct Cell { double norm = 0.0, level = 0.0, gr = 0.0; juce::String display; };
// the draft's in_at_gr ([{norm, display, gr_db_by_level: {"-12": x}}]) or the record's cells ([{norm, display, level_dbfs, gr_db}])
inline std::vector<Cell> cellsOf (const juce::var& band)
{
    std::vector<Cell> out;
    if (const auto* a = band.getProperty ("in_at_gr", {}).getArray())
        for (const auto& p : *a)
            if (const auto* lv = p.getProperty ("gr_db_by_level", {}).getDynamicObject())
                for (const auto& l : lv->getProperties())
                    if (! l.value.isVoid() && l.value.toString().isNotEmpty()) out.push_back ({ (double) p.getProperty ("norm", 0.0), l.name.toString().getDoubleValue(), (double) l.value, p.getProperty ("display", "").toString() });
    if (const auto* a = band.getProperty ("cells", {}).getArray())
        for (const auto& c : *a) out.push_back ({ (double) c.getProperty ("norm", 0.0), (double) c.getProperty ("level_dbfs", 0.0), (double) c.getProperty ("gr_db", 0.0), c.getProperty ("display", "").toString() });
    return out;
}
inline std::vector<std::pair<double, double>> atLevel (const std::vector<Cell>& cells, double level)   // (norm, gr) sorted by norm
{
    std::vector<std::pair<double, double>> v; for (const auto& c : cells) if (std::abs (c.level - level) < 0.01) v.push_back ({ c.norm, c.gr });
    std::sort (v.begin(), v.end()); return v;
}
// THE PREDICTED SETTING for a gain-reduction ask at a level: the norm where the ladder's GR crosses g, interpolated (either direction)
inline std::optional<double> predictNorm (const std::vector<Cell>& cells, double level, double g)
{
    const auto v = atLevel (cells, level);
    for (size_t i = 1; i < v.size(); ++i)
    {
        const double a = v[i - 1].second, b = v[i].second;
        if ((a - g) * (b - g) <= 0.0 && std::abs (b - a) > 1e-9) return v[i - 1].first + (g - a) / (b - a) * (v[i].first - v[i - 1].first);
    }
    return std::nullopt;
}
inline double maxGr (const std::vector<Cell>& cells) { double m = 0.0; for (const auto& c : cells) m = std::max (m, c.gr); return m; }
inline Part directionPart (const std::vector<Cell>& cells, double level = -12.0)
{
    const auto v = atLevel (cells, level);
    if (v.size() < 3) return { "direction", "not_tested", juce::String ((int) v.size()) + " position(s) at " + juce::String (level, 0) + " dBFS" };
    bool up = true, down = true;
    for (size_t i = 1; i < v.size(); ++i) { if (v[i].second < v[i - 1].second - 0.5) up = false; if (v[i].second > v[i - 1].second + 0.5) down = false; }
    return up || down ? Part { "direction", "pass", "" } : Part { "direction", "fail", "gain reduction reverses with the threshold" };
}
// THE THRESHOLD'S ACCURACY: per position, the tone RMS level where GR reaches 1 dB (interpolated between the measured levels) against its dB
// display. The display must TRACK the real threshold 1:1 - every position's (onset - display) within kThresholdTolDb of their median, the
// median being the display's reference (0 = calibrated in dBFS; Lindell MBC reads about -21: a dBu-style scale, said); one position alone
// passes only within kThresholdTolDb outright. Drawmer 1973's gap runs 18..29 dB over its positions: the display does not track - fail
inline Part thresholdPart (const std::vector<Cell>& cells)
{
    std::map<double, std::vector<std::pair<double, double>>> byNorm; std::map<double, juce::String> disp;   // norm -> (level, gr)
    for (const auto& c : cells) { byNorm[c.norm].push_back ({ c.level, c.gr }); disp[c.norm] = c.display; }
    std::vector<std::pair<double, juce::String>> offsets;   // onset - display, with the display
    for (auto& [n, pts] : byNorm)
    {
        const auto shown = numberOf (disp[n]); if (! shown) continue;
        std::sort (pts.begin(), pts.end());
        for (size_t i = 1; i < pts.size(); ++i) if (pts[i - 1].second < 1.0 && pts[i].second >= 1.0)
        { const double t = (1.0 - pts[i - 1].second) / (pts[i].second - pts[i - 1].second); offsets.push_back ({ pts[i - 1].first + t * (pts[i].first - pts[i - 1].first) + kSineRmsDb - *shown, disp[n] }); break; }
    }
    if (offsets.empty()) return { "threshold", "not_tested", "no position with a dB display compresses inside the measured levels" };
    if (offsets.size() == 1)
        return std::abs (offsets[0].first) <= kThresholdTolDb ? Part { "threshold", "pass", "'" + offsets[0].second + "' starts within " + juce::String (std::abs (offsets[0].first), 1) + " dB (one position)" }
                                                             : Part { "threshold", "not_tested", "one position, " + juce::String (offsets[0].first, 1) + " dB from its display: a reference offset cannot be told from one" };
    std::vector<double> v; for (const auto& o : offsets) v.push_back (o.first); std::sort (v.begin(), v.end());
    const double median = v[v.size() / 2];
    for (const auto& o : offsets) if (std::abs (o.first - median) > kThresholdTolDb)
        return { "threshold", "fail", "the display does not track the real threshold: '" + o.second + "' is " + juce::String (o.first - median, 1) + " dB off the others (" + juce::String (v.front(), 1) + ".." + juce::String (v.back(), 1) + ")" };
    return { "threshold", "pass", juce::String ((int) offsets.size()) + " positions track 1:1" + (std::abs (median) > kThresholdTolDb ? " with a display reference of " + juce::String (median, 1) + " dB" : juce::String()) };
}
// THE PER-BAND TONE CHECK's verdict: every ask measured, each within 0.5 dB
inline Part gainPart (const juce::var& tonecheck)
{
    const auto* a = tonecheck.getArray(); if (a == nullptr || a->isEmpty()) return { "gain", "not_tested", "no per-band tone check yet (Sean's run)" };
    juce::String w; int n = 0;
    for (const auto& x : *a)
    {
        const double asked = (double) x.getProperty ("asked_db", 0.0); const auto got = x.getProperty ("gr_db", {});
        if (got.isVoid() || got.toString().isEmpty()) { w = "the " + juce::String (asked, 0) + " dB ask did not measure (" + x.getProperty ("why", "").toString() + ")"; break; }
        ++n; if (std::abs ((double) got - asked) > kGainTolDb) { w = "asked " + juce::String (asked, 0) + " dB, measured " + juce::String ((double) got, 2); break; }
    }
    if (w.isNotEmpty()) return { "gain", "fail", w };
    return { "gain", "pass", juce::String (n) + " ask(s) within " + juce::String (kGainTolDb, 1) + " dB" };
}
// THE MEASURED CROSSOVER (Kathy, 16 Oct: no crossover type assumed): the frequency where the two neighbouring bands' responses are EQUAL.
// Two ways to get each band's response, both with every threshold open:
//   - SOLO (the unit has a solo per band): each neighbour soloed in turn - its response, dB per tone;
//   - ISOLATED (no solo): each neighbour compressed alone at its deep end against the unit open - its cut per tone, scaled by its own
//     deepest cut (normalisedCut), so two bands compressed by different depths are compared as the share of the spectrum each owns.
// crossingHz: between loHz and hiHz, the first tone where (a - b) changes sign, interpolated on log frequency.
inline std::vector<std::pair<double, double>> normalisedCut (std::vector<std::pair<double, double>> cut)
{
    double deepest = 0.0; for (const auto& [f, c] : cut) deepest = std::min (deepest, c);
    if (deepest > -0.5) return {};   // cut nothing: no share to compare
    for (auto& [f, c] : cut) c = c / deepest;   // 1 = fully the band's, 0 = none of it
    return cut;
}
inline std::optional<double> crossingHz (const std::vector<std::pair<double, double>>& a, const std::vector<std::pair<double, double>>& b, double loHz, double hiHz)
{
    std::map<double, double> bOf; for (const auto& [f, v] : b) bOf[f] = v;
    std::vector<std::pair<double, double>> d;   // (hz, a - b) on the tones both carry, inside the window
    for (const auto& [f, v] : a) if (f >= loHz && f <= hiHz) if (const auto it = bOf.find (f); it != bOf.end()) d.push_back ({ f, v - it->second });
    std::sort (d.begin(), d.end());
    for (size_t i = 1; i < d.size(); ++i)
        if ((d[i - 1].second > 0.0) != (d[i].second > 0.0))
        {
            const double t = d[i - 1].second / (d[i - 1].second - d[i].second);
            return std::pow (2.0, std::log2 (d[i - 1].first) + t * (std::log2 (d[i].first) - std::log2 (d[i - 1].first)));
        }
    return std::nullopt;
}
inline Part crossoverPart (const juce::var& crossovers, int band)
{
    const auto* a = crossovers.getArray();
    if (a == nullptr) return { "crossover", "not_tested", "no crossover measurement yet (Sean's run)" };
    if (a->isEmpty()) return { "crossover", "not_applicable", "no crossover control" };
    int n = 0; juce::String w;
    for (const auto& x : *a)
    {
        if ((int) x.getProperty ("band_below", -1) != band && (int) x.getProperty ("band_above", -1) != band) continue;   // the crossovers that bound this band
        if (const auto* ps = x.getProperty ("positions", {}).getArray())
            for (const auto& p : *ps)
            {
                const auto shown = numberOf (p.getProperty ("display", "").toString(), true); const auto got = p.getProperty ("measured_hz", {});
                if (! shown || got.isVoid() || got.toString().isEmpty()) continue;
                ++n; const double oct = std::abs (std::log2 ((double) got / *shown));
                if (oct > kCrossoverOct) w = x.getProperty ("control", "").toString() + " '" + p.getProperty ("display", "").toString() + "' measured " + juce::String ((double) got, 0) + " Hz";
            }
    }
    if (w.isNotEmpty()) return { "crossover", "fail", w };
    if (n == 0) return { "crossover", "not_applicable", "no crossover bounds this band" };
    return { "crossover", "pass", juce::String (n) + " position(s) within 1/6 octave" };
}
inline bool makeupPasses (const juce::var& makeup)
{
    const auto* ps = makeup.getProperty ("points", {}).getArray(); if (ps == nullptr || ps->isEmpty()) return false;
    for (const auto& p : *ps) { const auto d = p.getProperty ("display_delta_db", {}), m = p.getProperty ("measured_delta_db", {}); if (d.isVoid() || m.isVoid() || std::abs ((double) d - (double) m) > kMakeupTolDb) return false; }
    return true;
}

// THE JUDGE: the draft's bands (identity / range / direction / threshold) + the v1 record's per-band tone checks, make-ups and crossovers
inline Result judge (const juce::var& draft, const juce::var& v1)
{
    Result R; R.product = draft.getProperty ("plugin", {}).getProperty ("name", "").toString(); R.pluginId = draft.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString();
    std::map<int, juce::var> v1Bands; if (const auto* vb = v1.getProperty ("bands", {}).getArray()) for (const auto& b : *vb) v1Bands[(int) b.getProperty ("band", 0)] = b;
    const auto crossovers = v1.isObject() ? v1.getProperty ("crossovers", {}) : juce::var();
    if (const auto* bs = draft.getProperty ("bands", {}).getArray())
        for (const auto& b : *bs)
        {
            BandResult br; br.band = (int) b.getProperty ("band", 0); br.control = b.getProperty ("threshold_control", "").toString(); br.toneHz = (double) b.getProperty ("centre_hz", 0.0);
            const auto cells = cellsOf (b);
            br.parts.push_back (br.control.isNotEmpty() ? Part { "identity", "pass", b.getProperty ("pairing", "").toString() } : Part { "identity", "fail", "no threshold control paired with the band by measurement" });
            const double mg = maxGr (cells);
            br.parts.push_back (cells.empty() ? Part { "range", "not_tested", "no ladder" } : mg >= kMaxAskDb ? Part { "range", "pass", juce::String (mg, 1) + " dB" } : Part { "range", "fail", "reaches " + juce::String (mg, 1) + " dB, short of " + juce::String (kMaxAskDb, 0) });
            br.parts.push_back (directionPart (cells));
            const auto vb = v1Bands.count (br.band) ? v1Bands[br.band] : juce::var();
            {   // the threshold over the draft's levels plus the v1 measuring's wider grid (when it ran)
                auto all = cells; if (const auto* tcs = vb.getProperty ("threshold_cells", {}).getArray()) for (const auto& c : *tcs) all.push_back ({ (double) c.getProperty ("norm", 0.0), (double) c.getProperty ("level_dbfs", 0.0), (double) c.getProperty ("gr_db", 0.0), c.getProperty ("display", "").toString() });
                br.parts.push_back (thresholdPart (all));
            }
            br.parts.push_back (gainPart (vb.getProperty ("tonecheck", {})));
            br.parts.push_back (crossoverPart (crossovers, br.band));
            auto state = [&] (const char* n) { for (const auto& p : br.parts) if (p.name == n) return p.state; return juce::String(); };
            br.pass = state ("identity") == "pass" && state ("range") == "pass" && state ("direction") == "pass" && state ("threshold") != "fail"
                      && state ("gain") == "pass" && (state ("crossover") == "pass" || state ("crossover") == "not_applicable");
            br.makeupControl = vb.getProperty ("makeup", {}).getProperty ("control", "").toString();
            br.makeupOffered = br.pass && makeupPasses (vb.getProperty ("makeup", {}));
            R.pass = R.pass || br.pass;
            R.bands.push_back (br);
        }
    return R;
}
// what the existing data alone can say: the bands that pass every part a draft can judge (the rest wait for the v1 measuring)
inline bool passesSoFar (const BandResult& b)
{
    for (const auto& p : b.parts) { if (p.state == "fail") return false; if ((p.name == "identity" || p.name == "range" || p.name == "direction") && p.state != "pass") return false; }
    return true;
}

} // namespace ejmap::mbv1
