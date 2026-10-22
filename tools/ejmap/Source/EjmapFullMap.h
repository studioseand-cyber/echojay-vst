/*
  EjmapFullMap.h - THE WIZARD'S FULL MAP (Kathy, 22 Oct, decision 0): a tester's map in the SAME format as the map store (schema
  kMapSchemaString, controls with range / unit / anchors or labels / trust), so an upload is usable by the server and judged by the same
  trust rules as every other map. Until now the wizard saved "ej_param_map/wizard-0": the parameter names and nothing the role rules or
  the read-back judge can use (22 Oct recount: 272 store maps, 0 could reach "mapped").

  From the probe's --extract (probe_extract.h: the list rows, then each parameter's sweep through the shared corpus extractor), with the
  EJ Map app's own rules (EjmapAssignPanel.h's whole-surface sweep), nothing invented here:
    - a sweep that sanitizes (EjmapSweeper.h finishOutcome, the app's code) -> an ANCHORED control: anchors [value, norm], range, the
      display-declared unit family, identity_display
    - a sweep that is non-numeric -> a MODE control: each distinct text at the first norm it appeared
    - flat both ways, refused by the sanitizer, or degenerate -> a DECLINE with the reason
    - an empty name -> a decline (not addressable); the same exact name twice -> one control, duplicate, both indices
    - the TAPER (decision 7): "log" where the anchors follow a geometric curve (EjmapTrust.h detectTaper)
  trust "setread" (no human verified it). Provenance says "wizard" and carries NO machine id (contract: no hardware ID anywhere).
*/

#pragma once

#include "EjmapSchema.h"
#include "EjmapSweeper.h"
#include "EjmapTrust.h"
#include <juce_core/juce_core.h>
#include <map>
#include <set>
#include <vector>

namespace ejmap::fullmap
{

inline constexpr const char* kTesterId = "wizard";

struct XParam
{
    int index = -1; juce::String name, label; int steps = 0; bool discrete = false, automatable = true, meta = false;
    juce::String method; double defNorm = 0.0; juce::String defText; juce::Array<echojay::SweepPoint> points; bool swept = false;
};
struct Extract { bool done = false; std::vector<XParam> params; int setReads = 0; };

// the probe's --extract output: the --list-params rows ("<i>\t<name>\t<label>\t<steps>\t<discrete>\t<automatable>\t<meta>"), then the
// xparam / xpt lines, then "extract done"
inline Extract parseExtract (const juce::String& out)
{
    Extract x; std::map<int, size_t> at;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() >= 7 && f[0].containsOnly ("0123456789") && f[0].isNotEmpty())
        {
            XParam p; p.index = f[0].getIntValue(); p.name = f[1]; p.label = f[2]; p.steps = f[3].getIntValue(); p.discrete = f[4] == "1"; p.automatable = f[5] == "1"; p.meta = f[6] == "1";
            at[p.index] = x.params.size(); x.params.push_back (p); continue;
        }
        if (f.size() >= 2 && f[0] == "xparam")
        {   const auto it = at.find (f[1].getIntValue()); if (it == at.end()) continue;
            auto& p = x.params[it->second]; p.swept = true;
            for (int i = 2; i + 1 < f.size(); i += 2) { if (f[i] == "method") p.method = f[i + 1]; else if (f[i] == "def") p.defNorm = f[i + 1].getDoubleValue(); else if (f[i] == "deftext") p.defText = f[i + 1]; }
            continue; }
        if (f.size() >= 3 && f[0] == "xpt")
        {   const auto it = at.find (f[1].getIntValue()); if (it == at.end()) continue;
            x.params[it->second].points.add ({ (float) f[2].getDoubleValue(), f.size() >= 4 ? f[3] : juce::String() }); continue; }
        if (f.size() >= 2 && f[0] == "extract" && f[1] == "done") { x.done = true; for (int i = 2; i + 1 < f.size(); i += 2) if (f[i] == "setread") x.setReads = f[i + 1].getIntValue(); }
    }
    return x;
}

struct Built { juce::Array<NamedControl> controls; juce::Array<DeclineRecord> declines; int anchored = 0, modes = 0, tapered = 0, blankLabels = 0; };

// THE CONTROLS, by the EJ Map app's rules (see the header)
inline Built buildControls (const Extract& x)
{
    Built b; std::map<juce::String, int> nameCount;
    for (const auto& p : x.params) ++nameCount[p.name];
    std::set<juce::String> dupDone;
    for (const auto& p : x.params)
    {
        if (p.name.trim().isEmpty()) { b.declines.add ({ p.index, juce::String(), "empty parameter name: not addressable, and juce JSON writes an empty key it cannot re-read" }); continue; }
        if (nameCount[p.name] > 1)
        {
            if (! dupDone.insert (p.name).second) continue;
            NamedControl c; c.name = p.name; c.duplicate = true;
            for (const auto& o : x.params) if (o.name == p.name) c.indices.add (o.index);
            b.controls.add (c); continue;
        }
        if (! p.swept) { b.declines.add ({ p.index, p.name, "not swept (the extract ended before it)" }); continue; }
        if (p.method == "flat" || p.method == "flat_unretried")
        { b.declines.add ({ p.index, p.name, p.method == "flat" ? "flat both ways - unbuildable (the display carries no value information)" : "flat by getText; set-then-read not tried (the extract's time budget)" }); continue; }
        const auto sw = outcomeFromPoints (p.points, p.method.isNotEmpty() ? p.method : juce::String ("gettext"));
        NamedControl c; c.name = p.name; c.indices.add (p.index); c.trust = Trust::setread;
        if (sw.ok)
        {
            std::vector<trust::Anchor> ta;
            for (const auto& a : sw.anchors) { c.anchors.add ({ (double) a[1], (double) a[0] }); ta.push_back ({ (double) a[0], (double) a[1] }); }
            float lo = sw.anchors.getFirst()[0], hi = lo; for (const auto& a : sw.anchors) { lo = juce::jmin (lo, a[0]); hi = juce::jmax (hi, a[0]); }
            c.rangeLo = lo; c.rangeHi = hi; c.unit = sw.unitFamily; c.identityDisplay = sw.identityDisplay;
            std::sort (ta.begin(), ta.end(), [] (const trust::Anchor& u, const trust::Anchor& v) { return u.norm < v.norm; });
            c.taper = trust::detectTaper (ta); if (c.taper.isNotEmpty()) ++b.tapered;
            ++b.anchored; b.controls.add (c); continue;
        }
        if (sw.nonNumeric && ! sw.points.isEmpty())
        {
            // AN EMPTY LABEL IS NOT A LABEL (22 Oct, StandardCLIP's Clip Type shows "" at 0.30): juce::JSON writes the empty key and its own
            // parser refuses to read it back, so the whole map was unreadable and the recount lost the plugin - left out, said in the count
            c.kind = "mode"; juce::StringArray seen; int blank = 0;
            for (const auto& pt : sw.points) { if (pt.t.trim().isEmpty()) { ++blank; continue; } if (! seen.contains (pt.t)) { seen.add (pt.t); c.labels.add ({ pt.t, (double) pt.n }); } }
            b.blankLabels += blank;
            if (c.labels.isEmpty()) { b.declines.add ({ p.index, p.name, "a labelled switch whose every label is empty: nothing to address" }); continue; }
            ++b.modes; b.controls.add (c); continue;
        }
        b.declines.add ({ p.index, p.name, "unbuildable: " + sw.reason });
    }
    return b;
}

// THE MAP: the shared payload writer (EjmapSchema.h MapPayload), the store's format
inline juce::var buildMap (const juce::String& fp, const juce::String& format, const juce::String& uid, const juce::String& name, const juce::String& vendor,
                           const juce::String& version, const juce::String& category, const Extract& x, const Built& b, const juce::String& ejmapVersion)
{
    MapPayload m; m.fp = fp; m.category = category; m.mode = Mode::fast;
    m.identity.format = format; m.identity.uid = uid.toLowerCase(); m.identity.name = name; m.identity.vendor = vendor; m.identity.version = version; m.identity.paramCount = (int) x.params.size();
    m.controls = b.controls; m.declines = b.declines; m.declinesRecorded = true;
    m.provenance.testerId = kTesterId; m.provenance.machineId = {};   // no hardware ID anywhere (contract v1)
    m.provenance.ejmapVersion = ejmapVersion; m.provenance.extractorVersion = juce::String (echojay::ExtractorConfig().extractorVersion);
    m.provenance.pluginVersion = version; m.provenance.hostOs = juce::SystemStats::getOperatingSystemName();
    m.provenance.at = juce::Time::getCurrentTime().toISO8601 (true);
    return m.toVar();
}

// THE PARAMETER LIST of a wizard map, either format: wizard-0's "params" list, or a full map's controls (index / indices) and declines
inline std::vector<std::pair<int, juce::String>> paramsOf (const juce::var& map)
{
    std::vector<std::pair<int, juce::String>> ps; std::set<int> seen;
    auto add = [&] (int i, const juce::String& n) { if (i >= 0 && seen.insert (i).second) ps.push_back ({ i, n }); };
    if (const auto* a = map.getProperty ("params", {}).getArray()) for (const auto& p : *a) add ((int) p.getProperty ("index", -1), p.getProperty ("name", "").toString());
    if (const auto* co = map.getProperty ("controls", {}).getDynamicObject())
        for (const auto& c : co->getProperties())
        {   const auto n = c.value.getProperty ("name", c.name.toString()).toString();
            if (c.value.hasProperty ("index")) add ((int) c.value.getProperty ("index", -1), n);
            if (const auto* ix = c.value.getProperty ("indices", {}).getArray()) for (const auto& i : *ix) add ((int) i, n); }
    if (const auto* de = map.getProperty ("declines", {}).getArray()) for (const auto& d : *de) add ((int) d.getProperty ("index", -1), d.getProperty ("name", "").toString());
    std::sort (ps.begin(), ps.end());
    return ps;
}
// WHAT A CONTROL DEPENDS ON (Kathy, 22 Oct, decision 5): a pitch-shift draft's shifts[].depends_on, written onto the map's control of the
// same name (the bundle's copy - the wizard's own store and the map store are never rewritten by it)
inline juce::var withDepends (const juce::var& map, const juce::var& draft)
{
    auto m = juce::JSON::parse (juce::JSON::toString (map));   // a deep copy
    auto* co = m.getProperty ("controls", {}).getDynamicObject(); if (co == nullptr) return m;
    if (const auto* sh = draft.getProperty ("shifts", {}).getArray())
        for (const auto& c : *sh)
            if (const auto d = c.getProperty ("depends_on", {}); d.isObject())
                if (auto* ctl = co->getProperty (c.getProperty ("control", "").toString()).getDynamicObject())
                {   auto* o = new juce::DynamicObject(); o->setProperty ("control", d.getProperty ("control", "")); o->setProperty ("display", d.getProperty ("display", ""));
                    ctl->setProperty ("depends_on", juce::var (o)); }
    return m;
}
// a map the trust rules can use: the store's schema with at least one control
inline bool isFull (const juce::var& map)
{
    const auto sch = map.getProperty ("schema", "").toString();
    return (sch == kMapSchemaString || sch == "2.3") && map.getProperty ("controls", {}).getDynamicObject() != nullptr
           && map.getProperty ("controls", {}).getDynamicObject()->getProperties().size() > 0;
}

} // namespace ejmap::fullmap
