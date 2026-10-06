/*
  EjmapFrequency.h - FREQUENCY (Kathy's NEXT BUILD item A3, 6 Oct 2026; accuracy, data only, nothing exported).

  The tone check measured GR at the pick with a 997 Hz sine. The same process shape at 100 Hz and 5 kHz says whether the
  detector weighs frequency (a sidechain high-pass, a tilt, a de-essing emphasis): GR at each tone against the 997 Hz
  reading; a unit whose GR moves more than kFlagDb with frequency is flagged `frequency_dependent` (a sidechain filter
  suspected). The tone check's level is kept (the pick's L): only the frequency changes. PURE here (pins FQ1-FQ3).
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <optional>

namespace ejmap::frequency
{

inline const std::vector<double> kTonesHz { 100.0, 997.0, 5000.0 };
inline constexpr double kFlagDb = 1.0;

struct Verdict { bool ok = false; juce::String why; double refGrDb = 0.0; std::map<double, double> grDb, deltaDb; double maxAbsDeltaDb = 0.0; bool frequencyDependent = false; juce::StringArray unread; };
inline Verdict judge (const std::map<double, std::optional<double>>& grByHz)
{
    Verdict v;
    const auto ref = grByHz.find (997.0);
    if (ref == grByHz.end() || ! ref->second) { v.why = "no GR reading at 997 Hz (the reference)"; return v; }
    v.refGrDb = *ref->second; v.ok = true;
    for (const auto& [hz, gr] : grByHz)
    {
        if (! gr) { v.unread.add (juce::String (hz, 0) + " Hz"); continue; }
        v.grDb[hz] = *gr; const double d = std::round ((*gr - v.refGrDb) * 100.0) / 100.0; v.deltaDb[hz] = d;
        if (std::abs (d) > v.maxAbsDeltaDb) v.maxAbsDeltaDb = std::abs (d);
    }
    v.frequencyDependent = v.maxAbsDeltaDb > kFlagDb;
    v.why = v.frequencyDependent ? "GR moves " + juce::String (v.maxAbsDeltaDb, 2) + " dB with frequency (over " + juce::String (kFlagDb, 1) + "): a sidechain filter or emphasis suspected" : "GR within " + juce::String (kFlagDb, 1) + " dB across the tones (largest move " + juce::String (v.maxAbsDeltaDb, 2) + ")";
    return v;
}
inline juce::var verdictVar (const Verdict& v)
{
    auto* o = new juce::DynamicObject();
    if (! v.ok) { o->setProperty ("refused", v.why); return juce::var (o); }
    o->setProperty ("gr_997_db", std::round (v.refGrDb * 100.0) / 100.0);
    auto* g = new juce::DynamicObject(); auto* d = new juce::DynamicObject();
    for (const auto& [hz, gr] : v.grDb) { g->setProperty (juce::String (hz, 0), std::round (gr * 100.0) / 100.0); d->setProperty (juce::String (hz, 0), v.deltaDb.at (hz)); }
    o->setProperty ("gr_by_hz", juce::var (g)); o->setProperty ("delta_vs_997_db", juce::var (d)); o->setProperty ("max_abs_delta_db", std::round (v.maxAbsDeltaDb * 100.0) / 100.0);
    o->setProperty ("frequency_dependent", v.frequencyDependent); o->setProperty ("flag_db", kFlagDb); o->setProperty ("verdict", v.why);
    if (! v.unread.isEmpty()) o->setProperty ("unread", v.unread.joinIntoString (", "));
    return juce::var (o);
}

} // namespace ejmap::frequency
