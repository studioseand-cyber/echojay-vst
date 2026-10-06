/*
  EjmapMaterial.h - REAL MATERIAL (Kathy's NEXT BUILD item A2, 6 Oct 2026; accuracy, data only, nothing exported).

  The probe's --material mode renders a generated vocal, drum loop or full mix (probe_material.h: synthetic, nothing
  recorded, nothing copyrighted) through the unit at the tone check's writes - the pick included - twice: once quiet (the
  unit's static gain, where it should not compress) and once at the material's RMS. Per window it prints the input and
  output RMS on the input's clock. Here:
    GR(t)      = (quiet pass: out - in) - (loud window: out - in)          the gain reduction over time
    GR_pred(t) = the profile's curve AT THE PICK, read at the window's input RMS: in_at_gr[g] interpolated across the norm
                 axis to the pick's position gives in(g) for g = 1..12; inverted, the window's level gives g. Below the
                 1 dB point the prediction falls to 0 over 1 dB; above the deepest carried level it is clamped and flagged.
  The miss per window is GR(t) - GR_pred(t) over the windows whose input is above the material floor; the record keeps the
  median, the worst (largest |miss|), the 90th percentile of |miss|, the measured and predicted GR at the pass's own RMS,
  and the trace. A positive median means the unit reduces MORE than the sine curve says at that level (a peak-reading
  detector on peaky material); the attack and release show as the worst.

  PURE, pinned in RoundTripTest.cpp (MT1-MT6); the driver (runMaterial) runs the probe three times per product.
*/

#pragma once

#include "EjmapProfileExport.h"

namespace ejmap::material
{

inline constexpr double kFloorDbfs = -50.0;        // a window quieter than this says nothing about compression (a gap, a breath)
inline const juce::StringArray kKinds { "vocal", "drums", "mix" };

struct Window { double tMs = 0.0, inDb = 0.0, outDb = 0.0; };
struct Pass { bool ok = false; double targetDb = 0.0, inRmsDb = 0.0, outRmsDb = 0.0; std::vector<Window> windows; };
struct Parsed { bool ok = false; juce::String refused, kind; int latency = 0; Pass quiet, loud; long long nonfinite = 0; };

inline Parsed parseMaterial (const juce::String& out)
{
    Parsed p;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.isEmpty()) continue;
        if (f[0] == "refused") { p.refused = line.fromFirstOccurrenceOf ("refused ", false, false); return p; }
        if (f[0] == "material") { const int k = f.indexOf ("kind"); if (k >= 0 && k + 1 < f.size()) p.kind = f[k + 1]; }
        else if (f[0] == "config") { const int k = f.indexOf ("latency"); if (k >= 0 && k + 1 < f.size()) p.latency = f[k + 1].getIntValue(); }
        else if (f[0] == "mpass" && f.size() >= 8)
        {
            auto& ps = f[1] == "quiet" ? p.quiet : p.loud; ps.ok = true;
            const int kt = f.indexOf ("target_rms_db"), ki = f.indexOf ("in_rms_db"), ko = f.indexOf ("out_rms_db");
            if (kt >= 0) ps.targetDb = f[kt + 1].getDoubleValue(); if (ki >= 0) ps.inRmsDb = f[ki + 1].getDoubleValue(); if (ko >= 0) ps.outRmsDb = f[ko + 1].getDoubleValue();
        }
        else if (f[0] == "mwin" && f.size() >= 8)
        {
            auto& ps = f[1] == "quiet" ? p.quiet : p.loud; Window w;
            const int kt = f.indexOf ("t_ms"), ki = f.indexOf ("in_db"), ko = f.indexOf ("out_db");
            if (kt < 0 || ki < 0 || ko < 0) continue;
            w.tMs = f[kt + 1].getDoubleValue(); w.inDb = f[ki + 1].getDoubleValue(); w.outDb = f[ko + 1].getDoubleValue(); ps.windows.push_back (w);
        }
        else if (f[0] == "mdone") { const int k = f.indexOf ("nonfinite"); if (k >= 0 && k + 1 < f.size()) p.nonfinite = f[k + 1].getLargeIntValue(); }
    }
    p.ok = p.quiet.ok && p.loud.ok && ! p.loud.windows.empty();
    if (! p.ok && p.refused.isEmpty()) p.refused = "no complete quiet and loud pass in the trace";
    return p;
}

// THE CURVE AT THE PICK: in(g) for every g the profile carries, interpolated across the norm axis between the two curve
// points that bracket the pick (a point carrying null at g is skipped: that g is not carried there)
struct CurveAtPick { bool ok = false; juce::String why; std::map<int, double> inAtG; double norm = 0.0; };
inline CurveAtPick curveAtPick (const juce::var& profile, double pickNorm)
{
    CurveAtPick c; c.norm = pickNorm;
    const auto* pts = profile.getProperty ("amount", {}).getProperty ("curve", {}).getArray();
    if (pts == nullptr || pts->isEmpty()) { c.why = "the profile carries no amount curve"; return c; }
    int lo = -1, hi = -1;
    for (int i = 0; i < pts->size(); ++i) { const double n = (double) (*pts)[i].getProperty ("norm", 0.0); if (n <= pickNorm + 1e-9) lo = i; if (hi < 0 && n >= pickNorm - 1e-9) hi = i; }
    if (lo < 0) lo = hi; if (hi < 0) hi = lo;
    if (lo < 0) { c.why = "no curve point near the pick"; return c; }
    const auto& a = (*pts)[lo]; const auto& b = (*pts)[hi];
    const double na = (double) a.getProperty ("norm", 0.0), nb = (double) b.getProperty ("norm", 0.0);
    const double t = std::abs (nb - na) < 1e-9 ? 0.0 : juce::jlimit (0.0, 1.0, (pickNorm - na) / (nb - na));
    for (int g = 1; g <= 12; ++g)
    {
        const auto va = a.getProperty ("in_at_gr_dbfs", {}).getProperty (juce::String (g), juce::var()), vb = b.getProperty ("in_at_gr_dbfs", {}).getProperty (juce::String (g), juce::var());
        const bool ha = va.isDouble() || va.isInt(), hb = vb.isDouble() || vb.isInt();
        if (ha && hb) c.inAtG[g] = (1.0 - t) * (double) va + t * (double) vb;
        else if (ha && t < 1e-9) c.inAtG[g] = (double) va;
        else if (hb && t > 1.0 - 1e-9) c.inAtG[g] = (double) vb;
    }
    if (c.inAtG.empty()) { c.why = "the curve carries no level at the pick"; return c; }
    c.ok = true; return c;
}
// GR predicted at an input level from the curve at the pick (g over in, inverted); clamped above the deepest carried g (flagged)
struct Predicted { double grDb = 0.0; bool beyond = false; };
inline Predicted predictGr (const CurveAtPick& c, double inDb)
{
    Predicted p; if (c.inAtG.empty()) return p;
    const auto first = c.inAtG.begin(); const auto last = std::prev (c.inAtG.end());
    if (inDb <= first->second) { p.grDb = juce::jmax (0.0, (double) first->first - (first->second - inDb)); return p; }   // below the shallowest carried g: falls to 0 over its own dB count
    if (inDb > last->second) { p.grDb = (double) last->first; p.beyond = true; return p; }
    for (auto it = c.inAtG.begin(); std::next (it) != c.inAtG.end(); ++it)
    {
        const auto nx = std::next (it);
        if (inDb >= it->second && inDb <= nx->second)
        {
            const double span = nx->second - it->second;
            p.grDb = span < 1e-9 ? (double) it->first : (double) it->first + ((double) nx->first - (double) it->first) * (inDb - it->second) / span; return p;
        }
    }
    p.grDb = (double) last->first; return p;
}

struct Point { double tMs = 0.0, inDb = 0.0, grDb = 0.0, predDb = 0.0, missDb = 0.0; bool beyond = false; };
struct Result
{
    bool ok = false; juce::String refused, kind;
    double quietGainDb = 0.0, passInRmsDb = 0.0, passOutRmsDb = 0.0;
    double grAtPassRmsDb = 0.0, predAtPassRmsDb = 0.0;      // the whole loud pass's GR (quiet gain - pass gain) and the prediction at the pass RMS
    int windows = 0, counted = 0, beyond = 0;
    double medianMissDb = 0.0, worstMissDb = 0.0, p90AbsMissDb = 0.0, medianGrDb = 0.0, medianPredDb = 0.0;
    std::vector<Point> points;
};
inline Result derive (const Parsed& p, const CurveAtPick& c)
{
    Result r; r.kind = p.kind;
    if (! p.ok) { r.refused = p.refused; return r; }
    if (! c.ok) { r.refused = c.why; return r; }
    r.quietGainDb = p.quiet.outRmsDb - p.quiet.inRmsDb; r.passInRmsDb = p.loud.inRmsDb; r.passOutRmsDb = p.loud.outRmsDb;
    r.grAtPassRmsDb = std::round ((r.quietGainDb - (p.loud.outRmsDb - p.loud.inRmsDb)) * 100.0) / 100.0;
    r.predAtPassRmsDb = std::round (predictGr (c, p.loud.inRmsDb).grDb * 100.0) / 100.0;
    r.windows = (int) p.loud.windows.size();
    std::vector<double> misses, grs, preds, absm;
    for (const auto& w : p.loud.windows)
    {
        if (w.inDb < kFloorDbfs || w.inDb < -500.0 || w.outDb < -500.0) continue;
        Point pt; pt.tMs = w.tMs; pt.inDb = w.inDb; pt.grDb = r.quietGainDb - (w.outDb - w.inDb);
        const auto pr = predictGr (c, w.inDb); pt.predDb = pr.grDb; pt.beyond = pr.beyond; pt.missDb = pt.grDb - pt.predDb;
        if (pt.beyond) ++r.beyond;
        r.points.push_back (pt); misses.push_back (pt.missDb); grs.push_back (pt.grDb); preds.push_back (pt.predDb); absm.push_back (std::abs (pt.missDb));
    }
    r.counted = (int) misses.size();
    if (r.counted == 0) { r.refused = "no window above the material floor (" + juce::String (kFloorDbfs, 0) + " dBFS)"; return r; }
    auto med = [] (std::vector<double> v) { std::sort (v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]); };
    r.medianMissDb = std::round (med (misses) * 100.0) / 100.0; r.medianGrDb = std::round (med (grs) * 100.0) / 100.0; r.medianPredDb = std::round (med (preds) * 100.0) / 100.0;
    double worst = 0.0; for (double m : misses) if (std::abs (m) > std::abs (worst)) worst = m; r.worstMissDb = std::round (worst * 100.0) / 100.0;
    std::sort (absm.begin(), absm.end()); r.p90AbsMissDb = std::round (absm[(size_t) juce::jlimit (0, (int) absm.size() - 1, (int) std::floor (0.9 * (double) (absm.size() - 1)))] * 100.0) / 100.0;
    r.ok = true; return r;
}

inline juce::var resultVar (const Result& r)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("kind", r.kind);
    if (! r.ok) { o->setProperty ("refused", r.refused); return juce::var (o); }
    o->setProperty ("quiet_gain_db", std::round (r.quietGainDb * 100.0) / 100.0); o->setProperty ("pass_in_rms_dbfs", std::round (r.passInRmsDb * 100.0) / 100.0); o->setProperty ("pass_out_rms_dbfs", std::round (r.passOutRmsDb * 100.0) / 100.0);
    o->setProperty ("gr_at_pass_rms_db", r.grAtPassRmsDb); o->setProperty ("predicted_at_pass_rms_db", r.predAtPassRmsDb); o->setProperty ("miss_at_pass_rms_db", std::round ((r.grAtPassRmsDb - r.predAtPassRmsDb) * 100.0) / 100.0);
    o->setProperty ("windows", r.windows); o->setProperty ("windows_counted", r.counted); o->setProperty ("windows_beyond_curve", r.beyond);
    o->setProperty ("median_gr_db", r.medianGrDb); o->setProperty ("median_predicted_db", r.medianPredDb);
    o->setProperty ("median_miss_db", r.medianMissDb); o->setProperty ("worst_miss_db", r.worstMissDb); o->setProperty ("p90_abs_miss_db", r.p90AbsMissDb);
    juce::Array<juce::var> tr; for (const auto& pt : r.points) { auto* t = new juce::DynamicObject(); t->setProperty ("t_ms", pt.tMs); t->setProperty ("in_db", std::round (pt.inDb * 100.0) / 100.0); t->setProperty ("gr_db", std::round (pt.grDb * 100.0) / 100.0); t->setProperty ("pred_db", std::round (pt.predDb * 100.0) / 100.0); if (pt.beyond) t->setProperty ("beyond_curve", true); tr.add (juce::var (t)); }
    o->setProperty ("trace", tr);
    return juce::var (o);
}

} // namespace ejmap::material
