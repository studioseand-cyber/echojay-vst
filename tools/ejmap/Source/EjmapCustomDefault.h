/*
  EjmapCustomDefault.h - CUSTOM DEFAULTS, the DETECTION only (Kathy, 12 Oct; the MMorph finding). A plugin can open with a saved default
  (a user's, or a different preset library) instead of its factory state, and we would measure - and publish - that version.

  Given a REFERENCE instantiate state (the database's factory state for this plugin + version; today: another Mac's trace) and the
  OPENING state the probe printed at load (its "param <index> <norm> <name> <text>" lines, before anything is written), say whether
  they differ, control by control. Not a difference:
    - a value whose displayed text is the same (a stepped control's norm inside one step: MChorusMB "Sine" / "Sine");
    - a display-only or meter control (an analyzer view toggle, a VU readout: Pro-Q 3 "Analyzer Show Post-Processing", MC 77 "VU Meter L");
    - a NAME that differs with the same value (Melda's naming setting: 80 of the 12 Oct census's plugins).
  What to DO with a difference (write it back to the reference, or file custom_default and not measure) is Kathy's decision: not here.

  Census 12 Oct (cert-traces/2026-10-12-custom-defaults/): 299 plugins compared at the same version, 290 identical, 9 differ - 3 by
  the rules above, 6 for real, all MeldaProduction (MTurboCompMB, MMorph, MUnison, MFlangerMB, MTurboComp, MTuner). Pinned (CD-1..3).
*/
#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <vector>

namespace ejmap::customdefault
{
struct Param { double norm = 0.0; juce::String name, text; };
using Block = std::map<int, Param>;

// the probe's parameter block at load: "param<TAB>index<TAB>norm<TAB>name<TAB>text" lines, up to the first write / position / hold
inline Block parseBlock (const juce::String& trace)
{
    Block b;
    for (const auto& l : juce::StringArray::fromLines (trace))
    {
        if (b.empty() && (l.startsWith ("set\t") || l.startsWith ("write\t") || l.startsWith ("pos\t") || l.startsWith ("hold\t"))) return {};   // something was written first: not the opening state
        if (! l.startsWith ("param\t")) { if (! b.empty() && l.isNotEmpty() && ! l.startsWith ("#")) break; continue; }
        const auto t = juce::StringArray::fromTokens (l, "\t", "");
        if (t.size() < 5) continue;
        b[t[1].getIntValue()] = { t[2].getDoubleValue(), t[3], t[4] };
    }
    return b;
}
// controls that show or read, and never change the sound
inline bool displayOnly (const juce::String& name)
{
    auto tk = juce::StringArray::fromTokens (name.toLowerCase(), " -_/()[]:.,", ""); tk.removeEmptyStrings();
    for (const auto& w : tk)
        for (const char* k : { "meter", "meters", "vu", "ppm", "analyzer", "analyser", "show", "view", "zoom", "display", "gui", "scope", "spectrum" })   // never "resolution" (MMorph's spectral resolution is sound)
            if (w == k) return true;
    return false;
}
struct Difference { int index = -1; Param reference, opening; };
struct Verdict { bool comparable = false, differs = false; std::vector<Difference> diffs; int namesDiffer = 0, compared = 0; juce::String why; };
inline Verdict compare (const Block& reference, const Block& opening, double tol = 1e-3)
{
    Verdict v;
    if (reference.empty() || opening.empty()) { v.why = "no parameter block on one side"; return v; }
    v.comparable = true;
    for (const auto& [i, r] : reference)
    {
        const auto it = opening.find (i); if (it == opening.end()) continue;
        const auto& o = it->second; ++v.compared;
        if (r.name != o.name) ++v.namesDiffer;
        if (std::abs (r.norm - o.norm) <= tol) continue;
        if (r.text == o.text) continue;                                   // the same position as displayed
        if (displayOnly (r.name) || displayOnly (o.name)) continue;       // a view or a meter
        v.diffs.push_back ({ i, r, o });
    }
    v.differs = ! v.diffs.empty();
    v.why = v.differs ? juce::String ((int) v.diffs.size()) + " of " + juce::String (v.compared) + " control(s) open at another value than the reference"
                      : "the opening state matches the reference (" + juce::String (v.compared) + " control(s)" + (v.namesDiffer > 0 ? ", " + juce::String (v.namesDiffer) + " named differently" : juce::String()) + ")";
    return v;
}
} // namespace ejmap::customdefault
