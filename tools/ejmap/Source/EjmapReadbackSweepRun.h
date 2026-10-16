/*
  EjmapReadbackSweepRun.h - THE READ-BACK-ONLY SWEEP (Kathy, 16 Oct 2026, "OVERNIGHT ON THIS MAC"): the mapping sweep's per-control
  read-back (EjmapTrust.h readbackTargets -> write -> read -> restore) for maps that have none yet, headless, one probe load per map:
    - every anchored control at its three targets (25 / 50 / 75 % of its anchors' span), every mode control at two of its labels;
    - EchoJayProbe --text-at-norms-multi (one load, each control put back after its reads);
    - a reading counts only where the write LANDED and the probe's parameter at that index carries the map's control name: a control whose
      name differs here (another version, another build) is reported and left out, never judged against the wrong anchors.
  Writes <out>/phaseb/readback/sweep/<stem>.readback.json (ej_readback_sweep/1: product, version, controls[].displayAt { norm: text }), the
  shape --trust-levels already reads as a record (trustrun::loadRecords -> trust::readingsOfRecord), so the judging stays in one place.
  THE GUARD first (guardedPlugins: licence-bound dropped, said, never loaded), then the session's licence gate per product. Resumable: a
  product with its record is skipped. Stops between products at the run-all hour (EJ_RUNALL_UNTIL_MS) or on Ctrl-C / 07:00 (phasebStop).
    ejmap --readback-sweep --maps <maps folder> --names <file> --out <cert> [--probe <path>] [--ejmap-ledger <dir>]
  The maps folder is only read.
*/

#pragma once

#include "EjmapCertDriver.h"
#include "EjmapSelfGuardRun.h"
#include "EjmapTrust.h"

namespace ejmap::readback
{

struct Want { int index = -1; juce::String name, kind; std::vector<double> norms; };

// the plan for one map: anchored controls at readbackTargets, mode controls at their first two labels (by norm)
inline std::vector<Want> plan (const juce::var& map)
{
    std::vector<Want> out;
    const auto* co = map.getProperty ("controls", {}).getDynamicObject(); if (co == nullptr) return out;
    for (const auto& p : co->getProperties())
    {
        const auto& c = p.value; Want w; w.index = (int) c.getProperty ("index", -1); w.name = c.getProperty ("name", p.name.toString()).toString();
        w.kind = c.getProperty ("kind", "").toString(); if (w.index < 0) continue;
        if (w.kind == "anchored") for (const auto& t : trust::readbackTargets (trust::anchorsOf (c))) w.norms.push_back (t.norm);
        else if (w.kind == "mode")
            if (const auto* lo = c.getProperty ("labels", {}).getDynamicObject())
            {
                std::vector<double> ns; for (const auto& l : lo->getProperties()) ns.push_back ((double) l.value);
                std::sort (ns.begin(), ns.end()); ns.erase (std::unique (ns.begin(), ns.end()), ns.end());
                for (size_t i = 0; i < ns.size() && i < 2; ++i) w.norms.push_back (ns[i]);
            }
        if (! w.norms.empty()) out.push_back (w);
    }
    std::sort (out.begin(), out.end(), [] (const Want& a, const Want& b) { return a.index < b.index; });
    return out;
}
inline juce::String specOf (const std::vector<Want>& ws)
{
    juce::StringArray parts;
    for (const auto& w : ws) { juce::StringArray ns; for (double n : w.norms) ns.add (juce::String (n, 6)); parts.add (juce::String (w.index) + ":" + ns.joinIntoString (",")); }
    return parts.joinIntoString (";");
}

struct Read { double norm = 0.0; bool landed = false; juce::String text; };
struct Got { juce::String name; std::vector<Read> reads; };
// the probe's --text-at-norms-multi output: "textat ... index <i> name <n> ..." then "at <norm> landed|unlanded getValue .. text <t>"
inline std::map<int, Got> parse (const juce::String& out)
{
    std::map<int, Got> g; int cur = -1;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto t = juce::StringArray::fromTokens (line, "\t", "");
        if (t.size() >= 7 && t[0] == "textat" && t[3] == "index") { cur = t[4].getIntValue(); g[cur].name = t.size() > 6 ? t[6] : juce::String(); continue; }
        if (t.size() >= 3 && t[0] == "at" && cur >= 0)
        {
            Read r; r.norm = t[1].getDoubleValue(); r.landed = t[2] == "landed";
            const int ti = t.indexOf ("text"); if (ti >= 0 && ti + 1 < t.size()) r.text = t[ti + 1];
            g[cur].reads.push_back (r);
        }
    }
    return g;
}

// the record: per control, the readings that landed on the parameter that carries the map's name; everything else is said, not credited
inline juce::var record (const juce::var& map, const std::vector<Want>& ws, const std::map<int, Got>& got, const juce::String& installedVersion)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_readback_sweep/1"); o->setProperty ("product", map.getProperty ("identity", {}).getProperty ("name", ""));
    o->setProperty ("version", installedVersion); o->setProperty ("map_version", map.getProperty ("identity", {}).getProperty ("version", ""));
    o->setProperty ("map_fp", map.getProperty ("fp", ""));
    juce::Array<juce::var> cs, refused;
    for (const auto& w : ws)
    {
        const auto it = got.find (w.index);
        auto said = [&] (const juce::String& why) { auto* r = new juce::DynamicObject(); r->setProperty ("name", w.name); r->setProperty ("index", w.index); r->setProperty ("why", why); refused.add (juce::var (r)); };
        if (it == got.end()) { said ("the probe returned nothing for this index"); continue; }
        if (it->second.name.trim() != w.name.trim()) { said ("the parameter at this index is '" + it->second.name + "' here"); continue; }
        auto* da = new juce::DynamicObject(); int landed = 0, unlanded = 0;
        for (const auto& r : it->second.reads) { if (! r.landed) { ++unlanded; continue; } da->setProperty (juce::String (r.norm, 6), r.text); ++landed; }
        if (landed == 0) { said ("no write landed (" + juce::String (unlanded) + " tried)"); continue; }
        auto* c = new juce::DynamicObject(); c->setProperty ("name", w.name); c->setProperty ("index", w.index); c->setProperty ("kind", w.kind);
        c->setProperty ("displayAt", juce::var (da)); if (unlanded > 0) c->setProperty ("unlanded", unlanded); cs.add (juce::var (c));
    }
    o->setProperty ("controls", cs); o->setProperty ("not_read", refused);
    return juce::var (o);
}

} // namespace ejmap::readback

namespace ejmap::cert
{

inline int runReadbackSweep (const SweepOptions& opt, const juce::File& mapsDir, const juce::StringArray& names)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("READBACK: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const auto outDir = opt.out.getChildFile ("phaseb").getChildFile ("readback").getChildFile ("sweep"); outDir.createDirectory();
    const auto raw = opt.out.getChildFile ("phaseb").getChildFile ("readback").getChildFile ("raw"); raw.createDirectory();
    // THE GUARD before anything loads (Kathy, 16 Oct): licence-bound products are dropped here and never reach the probe
    const auto kept = guardedPlugins (names, false, "READBACK");
    std::map<juce::String, juce::var> mapOf;   // product -> its map (the newest file when there are several)
    std::map<juce::String, juce::Time> ageOf;
    for (const auto& f : mapsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto m = juce::JSON::parse (f.loadFileAsString()); const auto n = m.getProperty ("identity", {}).getProperty ("name", "").toString();
        if (n.isEmpty() || ! names.contains (n)) continue;
        if (! ageOf.count (n) || f.getLastModificationTime() > ageOf[n]) { mapOf[n] = m; ageOf[n] = f.getLastModificationTime(); }
    }
    const auto untilMs = juce::String (std::getenv ("EJ_RUNALL_UNTIL_MS") != nullptr ? std::getenv ("EJ_RUNALL_UNTIL_MS") : "").getLargeIntValue();
    int done = 0, skipped = 0, refused = 0, credited = 0;
    say ("READBACK: " + juce::String ((int) kept.size()) + " product(s) past the guard, maps read from " + mapsDir.getFullPathName() + " (read only)");
    for (const auto& desc : kept)
    {
        if (phasebStop() != 0) { say ("READBACK: stopped - the rest run on the resume"); return 130; }
        if (untilMs > 0 && juce::Time::currentTimeMillis() >= untilMs) { say ("READBACK: the run's hour reached - the rest run on the resume"); return 130; }
        if (! mapOf.count (desc.name)) { say ("READBACK: " + desc.name + " - no map in the folder"); ++refused; continue; }
        const auto& map = mapOf[desc.name];
        const auto stem = "AudioUnit_" + juce::String::toHexString (desc.uniqueId).toLowerCase() + "_" + desc.version;
        const auto recFile = outDir.getChildFile (stem + ".readback.json");
        if (recFile.existsAsFile()) { ++skipped; continue; }   // resumable
        if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("READBACK: " + desc.name + " - " + gate.stop); ++refused; continue; }
        const auto ws = readback::plan (map);
        if (ws.empty()) { say ("READBACK: " + desc.name + " - the map has no anchored or mode control to read back"); ++refused; continue; }
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId), "--text-at-norms-multi", readback::specOf (ws) };
        const auto r = runProbeCall (args, opt.timeoutMs);
        raw.getChildFile (stem + ".readback.txt").replaceWithText (r.out, false, false, "\n");
        if (r.kind == ChildResult::Kind::uiShown) { say ("READBACK: " + desc.name + ": a window appeared - stopped, nothing written"); return 5; }
        const auto got = readback::parse (r.out);
        if (got.empty()) { say ("READBACK: " + desc.name + " - the probe read nothing (raw kept: " + stem + ".readback.txt)"); ++refused; continue; }
        const auto rec = readback::record (map, ws, got, desc.version);
        const int n = rec.getProperty ("controls", {}).size(), nr = rec.getProperty ("not_read", {}).size();
        rec.getDynamicObject()->setProperty ("measuredAt", nowStamp());
        recFile.replaceWithText (juce::JSON::toString (rec) + "\n", false, false, "\n");
        say ("READBACK: " + desc.name + " -> " + juce::String (n) + " control(s) read, " + juce::String (nr) + " not read" + (desc.version != map.getProperty ("identity", {}).getProperty ("version", "").toString() ? " (installed " + desc.version + ", map " + map.getProperty ("identity", {}).getProperty ("version", "").toString() + ")" : juce::String()));
        ++done; credited += n;
    }
    say ("READBACK: done - " + juce::String (done) + " read (" + juce::String (credited) + " controls), " + juce::String (skipped) + " already read, " + juce::String (refused) + " not read (said above); nothing judged here - --trust-levels judges");
    return 0;
}

} // namespace ejmap::cert
