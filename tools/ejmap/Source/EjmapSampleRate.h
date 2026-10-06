/*
  EjmapSampleRate.h - SAMPLE RATE (Kathy's NEXT BUILD item A4, 6 Oct 2026; accuracy, data only, nothing exported).

  The tone check's process at the pick (its writes, its L, 997 Hz) with the plugin prepared at 44.1 and 96 kHz as well as 48:
  GR per rate against the 48 kHz reading. A move over kFlagDb (the tone check's own 0.5 dB bar) flags `rate_dependent` -
  the question Kathy asked: do profiles need a rate field? The spread: every k-th certified product so ten are measured
  (`spreadOf`), unless --only names them. PURE here (pins SR1-SR3).
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <optional>

namespace ejmap::samplerate
{

inline const std::vector<double> kRates { 44100.0, 48000.0, 96000.0 };
inline constexpr double kFlagDb = 0.5;
inline constexpr int kSpread = 10;

// the indices of a spread of n out of total, evenly spaced from the first (total <= n: all)
inline std::vector<int> spreadOf (int total, int n = kSpread)
{
    std::vector<int> out; if (total <= 0) return out;
    if (total <= n) { for (int i = 0; i < total; ++i) out.push_back (i); return out; }
    for (int i = 0; i < n; ++i) out.push_back ((int) std::llround ((double) i * (double) (total - 1) / (double) (n - 1)));
    return out;
}

struct Verdict { bool ok = false; juce::String why; double refGrDb = 0.0; std::map<double, double> grDb, deltaDb; std::map<double, int> latency; double maxAbsDeltaDb = 0.0; bool rateDependent = false; juce::StringArray unread; };
inline Verdict judge (const std::map<double, std::optional<double>>& grBySr)
{
    Verdict v;
    const auto ref = grBySr.find (48000.0);
    if (ref == grBySr.end() || ! ref->second) { v.why = "no GR reading at 48 kHz (the reference)"; return v; }
    v.refGrDb = *ref->second; v.ok = true;
    for (const auto& [sr, gr] : grBySr)
    {
        if (! gr) { v.unread.add (juce::String (sr / 1000.0, 1) + " kHz"); continue; }
        v.grDb[sr] = *gr; const double d = std::round ((*gr - v.refGrDb) * 100.0) / 100.0; v.deltaDb[sr] = d;
        if (std::abs (d) > v.maxAbsDeltaDb) v.maxAbsDeltaDb = std::abs (d);
    }
    v.rateDependent = v.maxAbsDeltaDb > kFlagDb;
    v.why = v.rateDependent ? "GR moves " + juce::String (v.maxAbsDeltaDb, 2) + " dB with the sample rate (over " + juce::String (kFlagDb, 1) + "): the profile would need a rate field for this unit" : "GR within " + juce::String (kFlagDb, 1) + " dB across 44.1 / 48 / 96 kHz (largest move " + juce::String (v.maxAbsDeltaDb, 2) + ")";
    return v;
}
inline juce::var verdictVar (const Verdict& v)
{
    auto* o = new juce::DynamicObject();
    if (! v.ok) { o->setProperty ("refused", v.why); return juce::var (o); }
    o->setProperty ("gr_48k_db", std::round (v.refGrDb * 100.0) / 100.0);
    auto* g = new juce::DynamicObject(); auto* d = new juce::DynamicObject(); auto* l = new juce::DynamicObject();
    for (const auto& [sr, gr] : v.grDb) { g->setProperty (juce::String (sr, 0), std::round (gr * 100.0) / 100.0); d->setProperty (juce::String (sr, 0), v.deltaDb.at (sr)); }
    for (const auto& [sr, lat] : v.latency) l->setProperty (juce::String (sr, 0), lat);
    o->setProperty ("gr_by_sr", juce::var (g)); o->setProperty ("delta_vs_48k_db", juce::var (d)); o->setProperty ("latency_by_sr", juce::var (l)); o->setProperty ("max_abs_delta_db", std::round (v.maxAbsDeltaDb * 100.0) / 100.0);
    o->setProperty ("rate_dependent", v.rateDependent); o->setProperty ("flag_db", kFlagDb); o->setProperty ("verdict", v.why);
    if (! v.unread.isEmpty()) o->setProperty ("unread", v.unread.joinIntoString (", "));
    return juce::var (o);
}

} // namespace ejmap::samplerate
