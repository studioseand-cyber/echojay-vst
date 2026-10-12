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
// --- THE REFERENCE AND THE ACTION (Kathy, 12 Oct, item 3) -----------------------------------------------------------------------------
// The AU's own factory preset was VALIDATED and REJECTED as the reference (cert-traces/2026-10-12-custom-defaults/factory_validation.txt:
// on 289 plugins that open identically on two Macs, the first factory preset equals the opening state in 93 of 198 that publish one,
// 47%; 91 publish none). The reference is therefore the opening block that INDEPENDENT MACS AGREE ON: trusted at 2 or more sources.
inline constexpr int kTrustedAgreeing = 2;
struct Reference { Block block; int agreeing = 0; juce::StringArray sources; };
inline bool trusted (const Reference& r) { return ! r.block.empty() && r.agreeing >= kTrustedAgreeing; }
inline juce::var referenceVar (const juce::String& pluginId, const Block& b, int agreeing, const juce::StringArray& sources)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("schema", "ej_instantiate_reference/0"); o->setProperty ("plugin_id", pluginId);
    o->setProperty ("agreeing", agreeing); juce::Array<juce::var> src; for (const auto& s : sources) src.add (s); o->setProperty ("sources", src);
    juce::Array<juce::var> rows; for (const auto& [i, p] : b) rows.add (juce::Array<juce::var> { i, p.norm, p.name, p.text }); o->setProperty ("block", rows);
    return juce::var (o);
}
inline Reference readReference (const juce::var& v)
{
    Reference r; r.agreeing = (int) v.getProperty ("agreeing", 0);
    if (const auto* s = v.getProperty ("sources", {}).getArray()) for (const auto& x : *s) r.sources.add (x.toString());
    if (const auto* rows = v.getProperty ("block", {}).getArray())
        for (const auto& row : *rows) if (const auto* a = row.getArray(); a != nullptr && a->size() >= 4) r.block[(int) (*a)[0]] = { (double) (*a)[1], (*a)[2].toString(), (*a)[3].toString() };
    return r;
}
enum class Act { measure, restore, customDefault };
struct Plan { Act act = Act::measure; juce::String writes, why; std::vector<Difference> diffs; };
// the writes that put every differing control back at the reference: "index:norm,..." (the probe's EJ_PROBE_PRESET / the child's --preset)
inline juce::String writesFor (const std::vector<Difference>& diffs)
{
    juce::StringArray w; for (const auto& d : diffs) w.add (juce::String (d.index) + ":" + juce::String (d.reference.norm, 6));
    return w.joinIntoString (",");
}
inline Plan plan (const Reference& ref, const Block& opening)
{
    Plan p;
    if (ref.block.empty()) { p.why = "no reference for this plugin and version: measured as it opens"; return p; }
    if (opening.empty()) { p.why = "no opening block read: measured as it opens (the measurement files any load failure itself)"; return p; }
    const auto v = compare (ref.block, opening);
    if (! v.differs) { p.why = "opens as the reference (" + v.why + ")"; return p; }
    p.diffs = v.diffs;
    if (! trusted (ref)) { p.act = Act::customDefault; p.why = "custom_default: " + v.why + "; the reference is not trusted (" + juce::String (ref.agreeing) + " source(s), " + juce::String (kTrustedAgreeing) + " needed)"; return p; }
    p.act = Act::restore; p.writes = writesFor (v.diffs); p.why = "opened with a custom default: " + v.why + "; restored to the reference by " + juce::String ((int) v.diffs.size()) + " write(s)";
    return p;
}
// THE VOTE (Kathy, 12 Oct, confirmed: trusted = 2+ independent Macs agree). Every plugin checked against a reference casts one: its
// opening block agrees or not. A plugin that opens IDENTICAL to a single-source reference is the second source - it is measured, and
// the upload marks the reference as agreed; one that DIFFERS from a single-source reference stays custom_default (held) until a third
// source settles it, and its own block is sent so that source can. Against a trusted reference the vote is information only.
struct Vote { bool cast = false, agrees = false; int agreeingBefore = 0; Block opening; };
inline Vote voteOf (const Reference& ref, const Block& opening)
{
    Vote v; if (ref.block.empty() || opening.empty()) return v;
    const auto c = compare (ref.block, opening); v.cast = c.comparable; v.agrees = c.comparable && ! c.differs; v.agreeingBefore = ref.agreeing;
    if (! v.agrees) v.opening = opening;
    return v;
}
inline juce::var voteVar (const Vote& v)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("agrees", v.agrees); o->setProperty ("agreeing_before", v.agreeingBefore);
    o->setProperty ("makes_trusted", v.agrees && v.agreeingBefore + 1 >= kTrustedAgreeing && v.agreeingBefore < kTrustedAgreeing);
    if (! v.agrees) { juce::Array<juce::var> rows; for (const auto& [i, p] : v.opening) rows.add (juce::Array<juce::var> { i, p.norm, p.name, p.text }); o->setProperty ("opening_block", rows); }
    return juce::var (o);
}
// the read-back after the writes: the block must now match the reference, or the plugin is filed custom_default
inline bool restoreConfirmed (const Reference& ref, const Block& readback) { const auto v = compare (ref.block, readback); return v.comparable && ! v.differs; }
// --- STRIPS (Kathy, 12 Oct, item 2): the restore writes and a strip section's own writes, merged ----------------------------------------
// A channel strip's section passes its own writes (engage, noise-off) as the probe's preset; that used to REPLACE the restore writes the
// strip was given. Merged: the restore writes first; a section write replaces only a control it sets itself; the record says which won.
struct MergedPreset { juce::String preset; juce::Array<juce::var> record; };
inline std::map<int, double> parsePreset (const juce::String& p)
{
    std::map<int, double> m;
    for (const auto& t : juce::StringArray::fromTokens (p, ",", "")) if (t.containsChar (':')) m[t.upToFirstOccurrenceOf (":", false, false).getIntValue()] = t.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
    return m;
}
inline MergedPreset mergePresets (const juce::String& restore, const juce::String& section)
{
    MergedPreset mp; const auto r = parsePreset (restore), s = parsePreset (section);
    std::map<int, double> out = r;
    for (const auto& [i, n] : s) out[i] = n;
    juce::StringArray parts; for (const auto& [i, n] : out) parts.add (juce::String (i) + ":" + juce::String (n, 6));
    mp.preset = parts.joinIntoString (",");
    for (const auto& [i, n] : r)
    {
        auto* o = new juce::DynamicObject(); o->setProperty ("index", i); o->setProperty ("restore_norm", n);
        if (const auto it = s.find (i); it != s.end()) { o->setProperty ("section_norm", it->second); o->setProperty ("won", "section"); }   // the section sets this control itself
        else o->setProperty ("won", "restore");
        mp.record.add (juce::var (o));
    }
    return mp;
}
} // namespace ejmap::customdefault
