#pragma once
/*
  THE RATIO LAW (Kathy, 19 Oct). Sean's server works out a certified compressor's gain reduction as GR = (input - threshold) x (1 - 1/ratio).
  A profile measured at ONE ratio (Tube-Tech CL 1B: 6:1 only) may use that maths at other ratios only once a spot check shows the ratio
  control behaves: the true ratio is the labelled one, and threshold and knee do not move with ratio.

  The check, on the Mac that holds the licence (ejmap --ratio-law, EjmapCertDriver.h): each ratio setting (the lowest, 2:1, 4:1, the
  highest - the measured ratio and duplicates skipped) at the tone check's g 2 and g 4 threshold positions, a steady tone at 3 input
  levels above that position's threshold (+6, +9, +12 dB), GR read after settling - the tone check's own probe call, the ratio write
  swapped. The scoring (here, pure): predicted (input - T) x (1 - 1/R_label), T from the EXISTING sweep at that position, against the
  measured GR; a point passes within kPassDb; points below half the knee are left out (excluded, never failed). Each ratio also reports
  its effective ratio from the slope and how far its threshold moved. The verdict never extrapolates past the lowest or highest ratio
  tested. Nothing here loads a plugin.
*/
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

namespace ejmap::ratiolaw
{
inline constexpr double kPassDb = 0.5;          // predicted vs measured GR, every point from the knee upward
inline constexpr double kThresholdMoveDb = 0.5; // a ratio's own threshold (from its slope) further than this from T: "threshold moved"
inline constexpr double kDefaultKneeDb = 6.0;   // a profile that states no knee: points less than 3 dB above T are not judged
inline constexpr double kSameRatio = 0.05;
inline const std::vector<double>& levelsAboveThreshold() { static const std::vector<double> v { 6.0, 9.0, 12.0 }; return v; }

// "6:1" / "6.0 : 1" / "6" -> 6; anything else -> nullopt
inline std::optional<double> ratioOfText (const juce::String& t)
{
    const auto s = t.trim().upToFirstOccurrenceOf (":", false, false).trim();
    if (s.isEmpty() || ! s.containsOnly ("0123456789.")) return std::nullopt;
    const double v = s.getDoubleValue(); return v >= 1.0 ? std::optional<double> (v) : std::nullopt;
}

// THE SETTINGS: the record's ratio control (its range, or its displayAt points) -> the lowest, 2:1, 4:1 and the highest, each with
// the norm that writes it; the measured ratio and duplicates skipped; a ratio outside the control's range is not offered
struct Setting { double ratio = 0.0, norm = 0.0; juce::String why; };
inline std::vector<Setting> settingsFor (const juce::var& ratioControl, double measuredRatio, juce::String& refused)
{
    std::vector<std::pair<double, double>> pts;   // (ratio, norm), ascending by norm
    if (const auto* o = ratioControl.getProperty ("displayAt", {}).getDynamicObject())
        for (const auto& p : o->getProperties()) if (const auto r = ratioOfText (p.value.toString())) pts.push_back ({ *r, p.name.toString().getDoubleValue() });
    std::sort (pts.begin(), pts.end(), [] (auto a, auto b) { return a.second < b.second; });
    const auto rg = ratioControl.getProperty ("range", {});
    const bool linear = (bool) rg.getProperty ("linear", false) && rg.hasProperty ("at0") && rg.hasProperty ("at1");
    if (! linear && pts.size() < 2) { refused = "the ratio control has neither a linear range nor two ratio readings to place a ratio by"; return {}; }
    auto normOf = [&] (double r) -> std::optional<double> {
        if (linear) { const double a = rg.getProperty ("at0", 0.0), b = rg.getProperty ("at1", 0.0); if (std::abs (b - a) < 1e-9) return std::nullopt;
                      const double n = (r - a) / (b - a); return n >= -1e-9 && n <= 1.0 + 1e-9 ? std::optional<double> (juce::jlimit (0.0, 1.0, n)) : std::nullopt; }
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {   const auto [r0, n0] = pts[i]; const auto [r1, n1] = pts[i + 1];
            if ((r - r0) * (r - r1) <= 0.0 && std::abs (r1 - r0) > 1e-9) return n0 + (n1 - n0) * (r - r0) / (r1 - r0); }
        return std::nullopt; };
    double lo = 1e9, hi = -1e9;
    if (linear) { lo = juce::jmin ((double) rg.getProperty ("at0", 0.0), (double) rg.getProperty ("at1", 0.0)); hi = juce::jmax ((double) rg.getProperty ("at0", 0.0), (double) rg.getProperty ("at1", 0.0)); }
    else for (const auto& [r, n] : pts) { lo = juce::jmin (lo, r); hi = juce::jmax (hi, r); }
    std::vector<Setting> out;
    for (const auto& [r, why] : std::vector<std::pair<double, const char*>> { { lo, "the lowest" }, { 2.0, "2:1" }, { 4.0, "4:1" }, { hi, "the highest" } })
    {
        if (std::abs (r - measuredRatio) < kSameRatio) continue;   // already measured
        bool dup = false; for (const auto& s : out) dup = dup || std::abs (s.ratio - r) < kSameRatio; if (dup) continue;
        if (const auto n = normOf (r)) out.push_back ({ r, *n, why });
    }
    return out;
}

// THE THRESHOLD FROM THE EXISTING SWEEP: at one curve position, the line through its (input at g dB, g) points for g >= 2 (the 1 dB point
// sits in the knee) - GR = s x (input - T); T is where that line meets 0 and s the measured slope (effective ratio 1 / (1 - s))
struct Line { bool ok = false; double T = 0.0, slope = 0.0; int points = 0; };
inline Line fitLine (const std::vector<std::pair<double, double>>& inGr)   // (input dBFS, GR dB)
{
    Line l; const int n = (int) inGr.size(); if (n < 2) return l;
    double sx = 0, sy = 0, sxx = 0, sxy = 0; for (const auto& [x, y] : inGr) { sx += x; sy += y; sxx += x * x; sxy += x * y; }
    const double den = n * sxx - sx * sx; if (std::abs (den) < 1e-12) return l;
    const double s = (n * sxy - sx * sy) / den, a = (sy - s * sx) / n; if (s <= 1e-6) return l;
    l.ok = true; l.slope = s; l.T = -a / s; l.points = n; return l;
}
inline Line thresholdAtPoint (const juce::var& curvePoint)
{
    std::vector<std::pair<double, double>> pts;
    if (const auto* o = curvePoint.getProperty ("in_at_gr_dbfs", {}).getDynamicObject())
        for (const auto& p : o->getProperties()) { const int g = p.name.toString().getIntValue(); if (g >= 2 && (p.value.isDouble() || p.value.isInt())) pts.push_back ({ (double) p.value, (double) g }); }
    return fitLine (pts);
}
// T at any norm: the two curve positions around it, each its own line, interpolated (never past the measured curve)
inline Line thresholdAt (const juce::var& amountCurve, double norm)
{
    std::vector<std::pair<double, Line>> ls;
    if (const auto* a = amountCurve.getArray()) for (const auto& p : *a) if (const auto l = thresholdAtPoint (p); l.ok) ls.push_back ({ (double) p.getProperty ("norm", 0.0), l });
    std::sort (ls.begin(), ls.end(), [] (auto x, auto y) { return x.first < y.first; });
    for (size_t i = 0; i < ls.size(); ++i)
    {
        if (std::abs (ls[i].first - norm) < 1e-6) return ls[i].second;
        if (i + 1 < ls.size() && ls[i].first < norm && norm < ls[i + 1].first)
        {   const double f = (norm - ls[i].first) / (ls[i + 1].first - ls[i].first); Line l; l.ok = true;
            l.T = ls[i].second.T + f * (ls[i + 1].second.T - ls[i].second.T); l.slope = ls[i].second.slope + f * (ls[i + 1].second.slope - ls[i].second.slope);
            l.points = juce::jmin (ls[i].second.points, ls[i + 1].second.points); return l; }
    }
    return {};
}

// THE SCORING
struct Point { double ratio = 0.0;          // the LABELLED ratio the write landed on (the plugin's own text, read back)
               double position = 0.0, T = 0.0;  // the threshold position (norm) and its T from the existing sweep
               double input = 0.0;               // dBFS, the same convention as the sweep (peak)
               std::optional<double> gr; };      // measured; none = no reading
struct Judged { Point p; double predicted = 0.0, error = 0.0; bool judged = false, pass = false; juce::String why; };
struct RatioResult { double ratio = 0.0; bool tested = false, pass = false, thresholdMoved = false; double worst = 0.0, effectiveRatio = 0.0, thresholdShift = 0.0; int judged = 0, excluded = 0; juce::String why; };
struct Verdict { juce::String status;   // "verified" | "partial" | "none"
                 std::vector<RatioResult> ratios; std::vector<Judged> points; std::vector<double> tested, safe; std::optional<std::pair<double, double>> safeBetween;
                 double worst = 0.0, kneeDb = kDefaultKneeDb, measuredRatio = 0.0; juce::String words; };

inline Judged judge (const Point& p, double kneeDb)
{
    Judged j; j.p = p;
    j.predicted = juce::jmax (0.0, (p.input - p.T) * (1.0 - 1.0 / p.ratio));
    if (! p.gr) { j.why = "no reading"; return j; }
    if (p.input - p.T < kneeDb / 2.0) { j.why = "below the knee (" + juce::String (p.input - p.T, 1) + " dB over T, the knee is " + juce::String (kneeDb, 1) + " dB): not judged"; return j; }
    j.judged = true; j.error = *p.gr - j.predicted; j.pass = std::abs (j.error) <= kPassDb;
    j.why = "predicted " + juce::String (j.predicted, 2) + ", measured " + juce::String (*p.gr, 2) + " dB";
    return j;
}

inline Verdict score (const std::vector<Point>& points, double kneeDbIn, double measuredRatio)
{
    Verdict v; v.kneeDb = kneeDbIn > 0.0 ? kneeDbIn : kDefaultKneeDb; v.measuredRatio = measuredRatio;
    std::map<double, std::vector<Judged>> byRatio;
    for (const auto& p : points) { auto j = judge (p, v.kneeDb); v.points.push_back (j); byRatio[std::round (p.ratio * 100.0) / 100.0].push_back (j); }
    for (const auto& [r, js] : byRatio)
    {
        RatioResult rr; rr.ratio = r;
        std::map<double, std::vector<std::pair<double, double>>> byPos;   // the effective slope per threshold position
        double shiftSum = 0.0; int shiftN = 0;
        for (const auto& j : js)
        {
            if (! j.judged) { ++rr.excluded; continue; }
            ++rr.judged; rr.worst = juce::jmax (rr.worst, std::abs (j.error)); byPos[j.p.position].push_back ({ j.p.input, *j.p.gr });
        }
        double slopeSum = 0.0; int slopeN = 0;
        for (const auto& [pos, pts] : byPos)
            if (const auto l = fitLine (pts); l.ok && pts.size() >= 2)
            {
                slopeSum += l.slope; ++slopeN;
                double T0 = 0.0; for (const auto& j : js) if (j.p.position == pos) { T0 = j.p.T; break; }
                shiftSum += l.T - T0; ++shiftN;
            }
        if (slopeN > 0) { const double s = slopeSum / slopeN; rr.effectiveRatio = s < 1.0 - 1e-6 ? 1.0 / (1.0 - s) : 1e9; }
        if (shiftN > 0) { rr.thresholdShift = shiftSum / shiftN; rr.thresholdMoved = std::abs (rr.thresholdShift) > kThresholdMoveDb; }
        rr.tested = rr.judged > 0;
        rr.pass = rr.tested && std::all_of (js.begin(), js.end(), [] (const Judged& j) { return ! j.judged || j.pass; });   // every judged point, one rule
        rr.why = ! rr.tested ? "no point above the knee - not tested (excluded, not failed)"
               : juce::String (rr.judged) + " point(s) judged, worst " + juce::String (rr.worst, 2) + " dB"
                 + (rr.effectiveRatio > 0.0 ? ", effective " + (rr.effectiveRatio > 1e8 ? juce::String ("limiting") : juce::String (rr.effectiveRatio, 2) + ":1") : juce::String())
                 + (shiftN > 0 ? ", threshold " + juce::String (rr.thresholdShift >= 0 ? "+" : "") + juce::String (rr.thresholdShift, 2) + " dB" + (rr.thresholdMoved ? " (MOVED)" : "") : juce::String());
        if (rr.tested) { v.tested.push_back (r); v.worst = juce::jmax (v.worst, rr.worst); if (rr.pass) v.safe.push_back (r); }
        v.ratios.push_back (rr);
    }
    std::sort (v.tested.begin(), v.tested.end()); std::sort (v.safe.begin(), v.safe.end());
    // SAFE BETWEEN: the widest run of TESTED ratios that all pass (adjacent in the tested list) - never past the lowest or highest tested
    {   size_t best0 = 0, best1 = 0; bool any = false;
        for (size_t i = 0; i < v.tested.size(); ++i)
        {
            size_t j = i; while (j < v.tested.size() && std::find (v.safe.begin(), v.safe.end(), v.tested[j]) != v.safe.end()) ++j;
            if (j > i && (! any || (j - i) > (best1 - best0))) { best0 = i; best1 = j; any = true; }
        }
        if (any && best1 - best0 >= 2) v.safeBetween = std::make_pair (v.tested[best0], v.tested[best1 - 1]); }
    v.status = v.tested.empty() ? juce::String ("none") : v.safe.size() == v.tested.size() ? juce::String ("verified") : v.safe.empty() ? juce::String ("none") : juce::String ("partial");
    auto list = [] (const std::vector<double>& xs) { juce::StringArray s; for (double x : xs) s.add (juce::String (x, x == std::floor (x) ? 0 : 1) + ":1"); return s.joinIntoString (", "); };
    v.words = v.status == "verified" ? "ratio law verified: " + list (v.tested) + " (every tested ratio within " + juce::String (kPassDb, 1) + " dB from the knee upward)"
            : v.status == "partial" ? "safe ratios: " + list (v.safe) + " of " + list (v.tested) + " tested"
            : v.tested.empty() ? juce::String ("nothing tested above the knee") : "no tested ratio follows the law (" + list (v.tested) + ")";
    return v;
}

// THE PROFILE FIELD (proposed): ratio_law {status, tested_ratios, safe_ratios, safe_between, worst_db, knee_db, measured_ratio, ...}.
// A profile WITHOUT it: the maths only at its measured ratio
inline juce::var block (const Verdict& v)
{
    auto arr = [] (const std::vector<double>& xs) { juce::Array<juce::var> a; for (double x : xs) a.add (x); return juce::var (a); };
    auto* o = new juce::DynamicObject();
    o->setProperty ("status", v.status); o->setProperty ("tested_ratios", arr (v.tested)); o->setProperty ("safe_ratios", arr (v.safe));
    o->setProperty ("safe_between", v.safeBetween ? arr ({ v.safeBetween->first, v.safeBetween->second }) : juce::var());
    o->setProperty ("worst_db", std::round (v.worst * 100.0) / 100.0); o->setProperty ("knee_db", v.kneeDb); o->setProperty ("measured_ratio", v.measuredRatio);
    auto* eff = new juce::DynamicObject(); auto* mv = new juce::DynamicObject();
    for (const auto& r : v.ratios) if (r.tested) { const auto k = juce::String (r.ratio, 2); eff->setProperty (k, r.effectiveRatio > 1e8 ? juce::var ("limiting") : juce::var (std::round (r.effectiveRatio * 100.0) / 100.0));
                                                   mv->setProperty (k, std::round (r.thresholdShift * 100.0) / 100.0); }
    o->setProperty ("effective_ratio", juce::var (eff)); o->setProperty ("threshold_shift_db", juce::var (mv));
    o->setProperty ("rule", "GR = (input - T) x (1 - 1/ratio), T from the measured sweep; pass within 0.5 dB at every point from the knee upward; never past the lowest or highest ratio tested");
    return juce::var (o);
}

// the law at the MEASURED ratio, from the existing sweep alone (no new load): its own in_at_gr points against (input - T) x (1 - 1/R)
inline Verdict lawAtMeasuredRatio (const juce::var& amountCurve, double measuredRatio, double kneeDb)
{
    std::vector<Point> pts;
    if (const auto* a = amountCurve.getArray())
        for (const auto& c : *a)
        {
            const auto l = thresholdAtPoint (c); if (! l.ok) continue;
            if (const auto* o = c.getProperty ("in_at_gr_dbfs", {}).getDynamicObject())
                for (const auto& p : o->getProperties())
                    if (p.value.isDouble() || p.value.isInt())
                    { Point q; q.ratio = measuredRatio; q.position = (double) c.getProperty ("norm", 0.0); q.T = l.T; q.input = (double) p.value; q.gr = (double) p.name.toString().getIntValue(); pts.push_back (q); }
        }
    return score (pts, kneeDb, measuredRatio);
}
} // namespace ejmap::ratiolaw
