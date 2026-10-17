/*
  EjmapReadbackSweepRun.h - THE READ-BACK-ONLY SWEEP (Kathy, 16 Oct 2026, "OVERNIGHT ON THIS MAC"): the mapping sweep's per-control
  read-back (EjmapTrust.h readbackTargets -> write -> read -> restore) for maps that have none yet, headless, one probe load per map:
    - every anchored control at its three targets (25 / 50 / 75 % of its anchors' span), every mode control at two of its labels;
    - EchoJayProbe --text-at-norms-multi (one load, each control put back after its reads);
    - a reading counts only on the probe's parameter at that index that carries the map's control name: a control whose name differs
      here (another version, another build) is reported and left out, never judged against the wrong anchors;
    - a write that LANDED is read at the norm written (displayAt); a write the plugin SNAPPED to its own step (Kathy, 17 Oct) is read at
      the value it took, getValue (snappedAt { taken: text }, snapped_from { taken: written }); readings at one taken value count once.
  ejmap --readback-rederive --maps <maps folder> --from <cert> --out <cert>: the same records rebuilt from the raw probe output kept in
  <from>/phaseb/readback/raw (nothing loaded); each keeps its product's version and measuredAt.
  Writes <out>/phaseb/readback/sweep/<stem>.readback.json (ej_readback_sweep/1: product, version, controls[].displayAt { norm: text }), the
  shape --trust-levels already reads as a record (trustrun::loadRecords -> trust::readingsOfRecord), so the judging stays in one place.
  THE GUARD first (guardedPlugins: licence-bound dropped, said, never loaded), then the session's licence gate per product. Resumable: a
  product with its record is skipped. Stops between products at the run-all hour (EJ_RUNALL_UNTIL_MS) or on Ctrl-C / 07:00 (phasebStop).
    ejmap --readback-sweep --maps <maps folder> --names <file> --out <cert> [--probe <path>] [--ejmap-ledger <dir>]
                           [--format VST3] [--at <file.json>]
  The maps folder is only read.
  --format VST3 (Kathy, 17 Oct, rulings 2): the VST3 maps, read through the probe's VST3 host. The products come from the ledger's
    scan-cache.xml (read only), through the same guard (PACE checked on the .vst3 bundle; a bundle that cannot be checked is held);
    the installed version is the one the probe's VST3 class reports ("vst3 class: ... version <v>"), never the scan cache's.
  --at <file.json> {product: {control: [norms]}} (rulings 2: the read-back at each old failure's own setting): only those controls are
    read, at their three targets AND the given norms, so a clean newer read-back lands on the failure itself.
*/

#pragma once

#include "EjmapCertDriver.h"
#include "EjmapSelfGuardRun.h"
#include "EjmapTrust.h"

namespace ejmap::readback
{

struct Want { int index = -1; juce::String name, kind; std::vector<double> norms; };

// the plan for one map: anchored controls at readbackTargets, mode controls at their first two labels (by norm)
inline std::vector<Want> plan (const juce::var& map, const juce::var& at = {})   // at: {control: [norms]} - only those controls, with those norms added
{
    std::vector<Want> out;
    for (const auto& [name, c] : trust::mapControls (map))   // both formats: the controls block and the older params block (17 Oct)
    {
        if (at.isObject() && ! at.hasProperty (name)) continue;
        Want w; w.index = (int) c.getProperty ("index", -1); w.name = name;
        w.kind = c.getProperty ("kind", "").toString(); if (w.index < 0) continue;
        if (w.kind == "anchored") for (const auto& t : trust::readbackTargets (trust::anchorsOf (c))) w.norms.push_back (t.norm);
        else if (w.kind == "mode")
            if (const auto* lo = c.getProperty ("labels", {}).getDynamicObject())
            {
                std::vector<double> ns; for (const auto& l : lo->getProperties()) ns.push_back ((double) l.value);
                std::sort (ns.begin(), ns.end()); ns.erase (std::unique (ns.begin(), ns.end()), ns.end());
                for (size_t i = 0; i < ns.size() && i < 2; ++i) w.norms.push_back (ns[i]);
            }
        if (const auto* extra = at.getProperty (name, {}).getArray(); extra != nullptr && ! w.norms.empty())
            for (const auto& n : *extra)
            {
                const double v = n.toString().getDoubleValue();
                if (std::none_of (w.norms.begin(), w.norms.end(), [&] (double x) { return std::abs (x - v) < 1e-6; })) w.norms.push_back (v);
            }
        std::sort (w.norms.begin(), w.norms.end());
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

// WHICH MAP (17 Oct: the overnight run read Meaw Chain / TB_Sibalance / Valhalla against their newer VST3 map): the probe loads the
// AudioUnit, so its map is the AudioUnit map of the INSTALLED version, else the newest AudioUnit map; a VST3 map is never used
struct MapFile { juce::var map; juce::int64 modMs = 0; juce::String file; };   // file: the map's file name (the anchor repair writes its copy under it)
inline const juce::var* pickMap (const std::vector<MapFile>& cands, const juce::String& installedVersion, const juce::String& format = "AudioUnit")
{
    const MapFile* best = nullptr; int bestRank = -1;
    for (const auto& c : cands)
    {
        const auto id = c.map.getProperty ("identity", {});
        if (id.getProperty ("format", "").toString() != format) continue;   // 17 Oct: the format the probe loads (AudioUnit, or VST3 with --format)
        const int rank = id.getProperty ("version", "").toString() == installedVersion ? 1 : 0;
        if (rank > bestRank || (rank == bestRank && c.modMs > best->modMs)) { best = &c; bestRank = rank; }
    }
    return best != nullptr ? &best->map : nullptr;
}

struct Read { double norm = 0.0; bool landed = false; juce::String text; double taken = -1.0; };   // taken: the probe's getValue after the write (-1 = not printed)
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
            const int gi = t.indexOf ("getValue"); if (gi >= 0 && gi + 1 < t.size()) r.taken = t[gi + 1].getDoubleValue();
            g[cur].reads.push_back (r);
        }
    }
    return g;
}

// the VST3 class line's installed version ("vst3 class: \"X\" (...)\tversion\t1.3.5.0"); empty when the probe did not print one
inline juce::String vst3VersionOf (const juce::String& out)
{
    for (const auto& line : juce::StringArray::fromLines (out))
        if (line.startsWith ("vst3 class: ") && line.contains ("\tversion\t")) return line.fromLastOccurrenceOf ("\tversion\t", false, false).trim();
    return {};
}
// THE VST3 CANDIDATES (17 Oct, rulings 2): the scan cache's VST3 entries for these names, through the same guard as the AudioUnits -
// the guard memory and vendor rules by name, the PACE check on the .vst3 bundle (one that cannot be checked is held, never assumed clear)
inline std::vector<juce::PluginDescription> guardedVst3 (const juce::StringArray& names, const juce::File& scanCache, const juce::String& tag, juce::StringArray* dropped = nullptr,
                                                         const juce::StringArray* allowNames = nullptr)
{
    const auto mem = selfguard::loadMemory (juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/scripts/self_guard_windows.json"));
    std::vector<juce::PluginDescription> ds; juce::StringArray found;
    if (auto xml = juce::XmlDocument::parse (scanCache))
        for (auto* e : xml->getChildWithTagNameIterator ("PLUGIN"))
        {
            juce::PluginDescription d; if (! d.loadFromXml (*e) || d.pluginFormatName != "VST3") continue;
            if (! names.contains (d.name) || found.contains (d.name)) continue;
            found.add (d.name);
            const juce::File bundle (d.fileOrIdentifier);
            juce::String pw; bool pace = true;
            if (! bundle.exists()) pw = "the bundle " + d.fileOrIdentifier + " is not on this Mac, so its PACE state cannot be checked: held";
            else if (cert::isPaceWrapped (bundle)) pw = "PACE-wrapped (" + bundle.getFileName() + ")"; else pace = false;
            const auto v = selfguard::judge (d.name, d.manufacturerName, "VST3|" + juce::String::toHexString (d.uniqueId).toLowerCase() + "|" + d.version, pace, pw, mem);
            if (v.drop && ! (allowNames != nullptr && allowNames->contains (d.name))) { std::cout << tag << ": GUARD dropped " << d.name << " (" << v.why << ")" << std::endl; if (dropped) dropped->add (d.name + "\t" + v.why); continue; }
            if (v.drop) std::cout << tag << ": GUARD let through (the allow list): " << d.name << " (" << v.why << ")" << std::endl;
            ds.push_back (d);
        }
    for (const auto& n : names) if (! found.contains (n)) std::cout << tag << ": no VST3 in the scan cache: " << n << std::endl;
    return ds;
}

// the record: per control, the readings that landed on the parameter that carries the map's name; everything else is said, not credited
inline juce::var record (const juce::var& map, const std::vector<Want>& ws, const std::map<int, Got>& got, const juce::String& installedVersion)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_readback_sweep/1"); o->setProperty ("product", map.getProperty ("identity", {}).getProperty ("name", ""));
    o->setProperty ("format", map.getProperty ("identity", {}).getProperty ("format", ""));   // the probe loads the format the picked map is (AudioUnit)
    o->setProperty ("version", installedVersion); o->setProperty ("map_version", map.getProperty ("identity", {}).getProperty ("version", ""));
    o->setProperty ("map_fp", map.getProperty ("fp", ""));
    juce::Array<juce::var> cs, refused;
    for (const auto& w : ws)
    {
        const auto it = got.find (w.index);
        auto said = [&] (const juce::String& why) { auto* r = new juce::DynamicObject(); r->setProperty ("name", w.name); r->setProperty ("index", w.index); r->setProperty ("why", why); refused.add (juce::var (r)); };
        if (it == got.end()) { said ("the probe returned nothing for this index"); continue; }
        if (it->second.name.trim() != w.name.trim()) { said ("the parameter at this index is '" + it->second.name + "' here"); continue; }
        auto* da = new juce::DynamicObject(); auto* sa = new juce::DynamicObject(); auto* sf = new juce::DynamicObject();
        int landed = 0, unlanded = 0, snapped = 0; std::vector<double> seen;
        for (const auto& r : it->second.reads) if (r.landed && trust::firstAtTaken (seen, r.norm)) { da->setProperty (juce::String (r.norm, 6), r.text); ++landed; }
        for (const auto& r : it->second.reads)   // then the snapped writes: a value a landed write already read is not read again
        {
            if (r.landed) continue;
            ++unlanded;
            if (r.taken < 0.0 || r.text.trim().isEmpty() || ! trust::firstAtTaken (seen, r.taken)) continue;   // no value printed, or one already read
            sa->setProperty (juce::String (r.taken, 6), r.text); sf->setProperty (juce::String (r.taken, 6), r.norm); ++snapped;
        }
        if (landed + snapped == 0) { said ("no write landed (" + juce::String (unlanded) + " tried)"); continue; }
        auto* c = new juce::DynamicObject(); c->setProperty ("name", w.name); c->setProperty ("index", w.index); c->setProperty ("kind", w.kind);
        c->setProperty ("displayAt", juce::var (da)); if (unlanded > 0) c->setProperty ("unlanded", unlanded);
        if (snapped > 0) { c->setProperty ("snappedAt", juce::var (sa)); c->setProperty ("snapped_from", juce::var (sf)); }
        cs.add (juce::var (c));
    }
    o->setProperty ("controls", cs); o->setProperty ("not_read", refused);
    return juce::var (o);
}

} // namespace ejmap::readback

namespace ejmap::cert
{

inline int runReadbackSweep (const SweepOptions& opt, const juce::File& mapsDir, const juce::StringArray& names, const juce::String& format = "AudioUnit", const juce::var& at = {},
                             const juce::StringArray& allow = {})
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("READBACK: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const auto outDir = opt.out.getChildFile ("phaseb").getChildFile ("readback").getChildFile ("sweep"); outDir.createDirectory();
    const auto raw = opt.out.getChildFile ("phaseb").getChildFile ("readback").getChildFile ("raw"); raw.createDirectory();
    // THE GUARD before anything loads (Kathy, 16 Oct): licence-bound products are dropped here and never reach the probe
    const auto kept = format == "VST3" ? readback::guardedVst3 (names, opt.ledger.getChildFile ("scan-cache.xml"), "READBACK", nullptr, &allow) : guardedPlugins (names, false, "READBACK", nullptr, &allow);
    std::map<juce::String, std::vector<readback::MapFile>> mapsOf;   // product -> every map file it has (readback::pickMap chooses)
    for (const auto& f : mapsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto m = juce::JSON::parse (f.loadFileAsString()); const auto n = m.getProperty ("identity", {}).getProperty ("name", "").toString();
        if (n.isEmpty() || ! names.contains (n)) continue;
        mapsOf[n].push_back ({ m, f.getLastModificationTime().toMilliseconds(), f.getFileName() });
    }
    const auto untilMs = juce::String (std::getenv ("EJ_RUNALL_UNTIL_MS") != nullptr ? std::getenv ("EJ_RUNALL_UNTIL_MS") : "").getLargeIntValue();
    int done = 0, skipped = 0, refused = 0, credited = 0;
    say ("READBACK: " + juce::String ((int) kept.size()) + " product(s) past the guard, maps read from " + mapsDir.getFullPathName() + " (read only)");
    for (const auto& desc : kept)
    {
        if (phasebStop() != 0) { say ("READBACK: stopped - the rest run on the resume"); return 130; }
        if (untilMs > 0 && juce::Time::currentTimeMillis() >= untilMs) { say ("READBACK: the run's hour reached - the rest run on the resume"); return 130; }
        const auto* picked = mapsOf.count (desc.name) ? readback::pickMap (mapsOf[desc.name], desc.version, format) : nullptr;
        if (picked == nullptr) { say ("READBACK: " + desc.name + " - no " + format + " map in the folder"); ++refused; continue; }
        const auto map = *picked;
        const auto stem = format + "_" + juce::String::toHexString (desc.uniqueId).toLowerCase() + "_" + desc.version;
        const auto recFile = outDir.getChildFile (stem + ".readback.json");
        if (recFile.existsAsFile()) { ++skipped; continue; }   // resumable
        if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("READBACK: " + desc.name + " - " + gate.stop); ++refused; continue; }
        const auto ws = readback::plan (map, at.isObject() ? at.getProperty (desc.name, juce::var (new juce::DynamicObject())) : juce::var());
        if (ws.empty()) { say ("READBACK: " + desc.name + " - the map has no anchored or mode control to read back"); ++refused; continue; }
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId), "--text-at-norms-multi", readback::specOf (ws) };
        const auto r = runProbeCall (args, opt.timeoutMs);
        raw.getChildFile (stem + ".readback.txt").replaceWithText (r.out, false, false, "\n");
        if (r.kind == ChildResult::Kind::uiShown) { say ("READBACK: " + desc.name + ": a window appeared - stopped, nothing written"); return 5; }
        const auto got = readback::parse (r.out);
        if (got.empty()) { say ("READBACK: " + desc.name + " - the probe read nothing (raw kept: " + stem + ".readback.txt)"); ++refused; continue; }
        auto installed = desc.version;   // a VST3's installed version is the probe's, never the (older) scan cache's
        if (format == "VST3") { const auto pv = readback::vst3VersionOf (r.out); if (pv.isNotEmpty() && pv != installed) say ("READBACK: " + desc.name + " - installed " + pv + " (the scan cache said " + installed + ")"); if (pv.isNotEmpty()) installed = pv; }
        const auto rec = readback::record (map, ws, got, installed);
        const int n = rec.getProperty ("controls", {}).size(), nr = rec.getProperty ("not_read", {}).size();
        rec.getDynamicObject()->setProperty ("measuredAt", nowStamp());
        recFile.replaceWithText (juce::JSON::toString (rec) + "\n", false, false, "\n");
        say ("READBACK: " + desc.name + " -> " + juce::String (n) + " control(s) read, " + juce::String (nr) + " not read" + (installed != map.getProperty ("identity", {}).getProperty ("version", "").toString() ? " (installed " + installed + ", map " + map.getProperty ("identity", {}).getProperty ("version", "").toString() + ")" : juce::String()));
        ++done; credited += n;
    }
    say ("READBACK: done - " + juce::String (done) + " read (" + juce::String (credited) + " controls), " + juce::String (skipped) + " already read, " + juce::String (refused) + " not read (said above); nothing judged here - --trust-levels judges");
    return 0;
}

// THE RE-DERIVE (17 Oct, the stepped rule): every record in <from>/phaseb/readback/sweep rebuilt by readback::record from the raw probe
// output kept beside it, against the map the sweep picks (pickMap, the record's own installed version). Nothing is loaded; measuredAt
// is the original reading's, so the recency rule sees the reading's time, not this command's
inline int runReadbackRederive (const juce::File& mapsDir, const juce::File& from, const juce::File& out)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto inDir = from.getChildFile ("phaseb").getChildFile ("readback");
    const auto outDir = out.getChildFile ("phaseb").getChildFile ("readback").getChildFile ("sweep"); outDir.createDirectory();
    if (outDir == inDir.getChildFile ("sweep")) { say ("REDERIVE: --out must not be --from (the originals are kept)"); return 2; }
    std::map<juce::String, std::vector<readback::MapFile>> mapsOf;
    for (const auto& f : mapsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto m = juce::JSON::parse (f.loadFileAsString()); const auto n = m.getProperty ("identity", {}).getProperty ("name", "").toString();
        if (n.isNotEmpty()) mapsOf[n].push_back ({ m, f.getLastModificationTime().toMilliseconds() });
    }
    int done = 0, refused = 0, snappedControls = 0;
    for (const auto& f : inDir.getChildFile ("sweep").findChildFiles (juce::File::findFiles, false, "*.readback.json"))
    {
        const auto old = juce::JSON::parse (f.loadFileAsString());
        const auto product = old.getProperty ("product", "").toString(), version = old.getProperty ("version", "").toString();
        const auto rawFile = inDir.getChildFile ("raw").getChildFile (f.getFileName().upToLastOccurrenceOf (".json", false, false) + ".txt");
        const auto fmt = trust::formatOfRecord (old, f.getFileName()).isNotEmpty() ? trust::formatOfRecord (old, f.getFileName()) : juce::String ("AudioUnit");
        const auto* picked = mapsOf.count (product) ? readback::pickMap (mapsOf[product], version, fmt) : nullptr;
        if (picked == nullptr || ! rawFile.existsAsFile()) { say ("REDERIVE: " + product + " - " + (picked == nullptr ? "no " + fmt + " map" : juce::String ("no raw output")) + "; not rebuilt"); ++refused; continue; }
        auto rec = readback::record (*picked, readback::plan (*picked), readback::parse (rawFile.loadFileAsString()), version);
        rec.getDynamicObject()->setProperty ("measuredAt", old.getProperty ("measuredAt", ""));
        rec.getDynamicObject()->setProperty ("rederived_from", f.getFullPathName());
        for (const auto& c : *rec.getProperty ("controls", {}).getArray()) if (c.hasProperty ("snappedAt")) ++snappedControls;
        outDir.getChildFile (f.getFileName()).replaceWithText (juce::JSON::toString (rec) + "\n", false, false, "\n");
        ++done;
    }
    say ("REDERIVE: " + juce::String (done) + " record(s) rebuilt from raw (" + juce::String (snappedControls) + " control(s) with snapped writes), " + juce::String (refused) + " not rebuilt; nothing loaded");
    return 0;
}

// THE ANCHOR REPAIR (Kathy, 17 Oct, NEXT 2/3): the named controls' anchor tables rebuilt from their own displays, one guarded probe load
// per product, on a grid that leaves out every setting the read-back will judge (trust::repairGrid). A control the map lacks is added
// from the plan's index, but only if the probe's parameter there carries that name. The repaired copy of the map is written to
// <out>/maps/<its file> (the map store is never written); each control carries trust "repair" and a "repair" block saying how.
//   ejmap --anchor-repair --maps <dir> --plan <file.json> --out <dir> [--grid 0.0125] [--probe ..] [--ejmap-ledger ..]
//   plan: { product: { "format": "AudioUnit"|"VST3", "controls": { name: { "index": n (a control the map lacks), "hold_out": [norms] } } } }
namespace repair
{
inline juce::var repairedMap (const juce::var& map, const juce::String& name, int index, const trust::Rebuilt& r, const juce::var& how)
{
    auto copy = juce::JSON::parse (juce::JSON::toString (map));   // a deep copy: the map that was read is never changed
    auto* co = copy.getProperty ("controls", {}).getDynamicObject();
    if (co == nullptr) { copy.getDynamicObject()->setProperty ("controls", juce::var (new juce::DynamicObject())); co = copy.getProperty ("controls", {}).getDynamicObject(); }
    juce::Identifier key (name); juce::var prev;
    for (const auto& p : co->getProperties()) if (p.value.getProperty ("name", p.name.toString()).toString() == name) { key = p.name; prev = p.value; }
    auto* c = new juce::DynamicObject();
    c->setProperty ("name", name); c->setProperty ("index", index); c->setProperty ("kind", "anchored");
    juce::Array<juce::var> as; double lo = r.anchors.front().value, hi = lo;
    for (const auto& a : r.anchors) { as.add (juce::var (juce::Array<juce::var> { a.value, a.norm })); lo = std::min (lo, a.value); hi = std::max (hi, a.value); }
    c->setProperty ("range", juce::var (juce::Array<juce::var> { lo, hi }));
    c->setProperty ("unit", prev.isObject() && ! prev.getProperty ("unit", {}).isVoid() ? prev.getProperty ("unit", {}) : (r.unit.isEmpty() ? juce::var() : juce::var (r.unit)));
    c->setProperty ("anchors", juce::var (as)); c->setProperty ("trust", "repair");
    auto h = juce::JSON::parse (juce::JSON::toString (how));
    if (auto* ho = h.getDynamicObject()) { if (prev.isObject()) ho->setProperty ("previous_anchors", prev.getProperty ("anchors", {}).size()); else ho->setProperty ("added", true); }
    c->setProperty ("repair", h);
    co->setProperty (key, juce::var (c));
    juce::Array<juce::var> rc; if (const auto* a = copy.getProperty ("repaired_controls", {}).getArray()) rc = *a; rc.addIfNotAlreadyThere (name);
    copy.getDynamicObject()->setProperty ("repaired_controls", juce::var (rc));
    return copy;
}
} // namespace repair

inline int runAnchorRepair (const SweepOptions& opt, const juce::File& mapsDir, const juce::var& plan, double step)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("REPAIR: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const auto* po = plan.getDynamicObject(); if (po == nullptr) { say ("REPAIR: the plan is not an object"); return 2; }
    for (const auto* sub : { "maps", "repair", "raw" }) opt.out.getChildFile (sub).createDirectory();
    std::map<juce::String, std::vector<readback::MapFile>> mapsOf;
    for (const auto& f : mapsDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto m = juce::JSON::parse (f.loadFileAsString()); const auto n = m.getProperty ("identity", {}).getProperty ("name", "").toString();
        if (n.isNotEmpty()) mapsOf[n].push_back ({ m, f.getLastModificationTime().toMilliseconds(), f.getFileName() });
    }
    int built = 0, refused = 0;
    for (const auto* fmt : { "AudioUnit", "VST3" })
    {
        juce::StringArray names; for (const auto& p : po->getProperties()) if (p.value.getProperty ("format", "AudioUnit").toString() == fmt) names.add (p.name.toString());
        if (names.isEmpty()) continue;
        // THE GUARD before anything loads, as every other load we run
        const auto kept = juce::String (fmt) == "VST3" ? readback::guardedVst3 (names, opt.ledger.getChildFile ("scan-cache.xml"), "REPAIR") : guardedPlugins (names, false, "REPAIR");
        for (const auto& desc : kept)
        {
            if (phasebStop() != 0) { say ("REPAIR: stopped"); return 130; }
            if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("REPAIR: " + desc.name + " - " + gate.stop); ++refused; continue; }
            const auto* picked = mapsOf.count (desc.name) ? readback::pickMap (mapsOf[desc.name], desc.version, fmt) : nullptr;
            if (picked == nullptr) { say ("REPAIR: " + desc.name + " - no " + juce::String (fmt) + " map"); ++refused; continue; }
            juce::String file; for (const auto& mf : mapsOf[desc.name]) if (&mf.map == picked) file = mf.file;
            const auto ctrls = plan.getProperty (desc.name, {}).getProperty ("controls", {});
            std::map<juce::String, std::pair<int, std::vector<double>>> want;   // name -> (index, hold-out)
            juce::StringArray parts;
            std::map<juce::String, int> inMap; for (const auto& [n, c] : trust::mapControls (*picked)) inMap[n] = (int) c.getProperty ("index", -1);
            if (const auto* co = ctrls.getDynamicObject())
                for (const auto& p : co->getProperties())
                {
                    const auto n = p.name.toString(); const int idx = inMap.count (n) ? inMap[n] : (int) p.value.getProperty ("index", -1);
                    if (idx < 0) { say ("REPAIR: " + desc.name + " / " + n + " - not in the map and the plan gives no index"); ++refused; continue; }
                    std::vector<double> ho; if (const auto* h = p.value.getProperty ("hold_out", {}).getArray()) for (const auto& x : *h) ho.push_back (x.toString().getDoubleValue());
                    want[n] = { idx, ho };
                    juce::StringArray ns; for (double x : trust::repairGrid (step, ho)) ns.add (juce::String (x, 6));
                    parts.add (juce::String (idx) + ":" + ns.joinIntoString (","));
                }
            if (want.empty()) continue;
            const auto stem = juce::String (fmt) + "_" + juce::String::toHexString (desc.uniqueId).toLowerCase() + "_" + desc.version;
            juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId), "--text-at-norms-multi", parts.joinIntoString (";") };
            const auto r = runProbeCall (args, opt.timeoutMs);
            opt.out.getChildFile ("raw").getChildFile (stem + ".repair.txt").replaceWithText (r.out, false, false, "\n");
            if (r.kind == ChildResult::Kind::uiShown) { say ("REPAIR: " + desc.name + ": a window appeared - stopped, nothing written"); return 5; }
            const auto got = readback::parse (r.out);
            const juce::String callWhy = r.kind == ChildResult::Kind::timedOut ? "the probe timed out after " + juce::String (opt.timeoutMs / 1000) + " s (--timeout)"
                                       : r.kind == ChildResult::Kind::signaled ? juce::String ("the probe died (signal)") : r.kind == ChildResult::Kind::exited && r.code != 0 ? "the probe exited " + juce::String (r.code) : juce::String();
            auto map = *picked; auto* rec = new juce::DynamicObject(); rec->setProperty ("product", desc.name); rec->setProperty ("format", fmt);
            rec->setProperty ("version", desc.version); rec->setProperty ("measuredAt", nowStamp()); auto* res = new juce::DynamicObject();
            for (const auto& [n, w] : want)
            {
                const auto it = got.find (w.first);
                auto said = [&] (const juce::String& why) { say ("REPAIR: " + desc.name + " / " + n + " - " + why); res->setProperty (n, "not built: " + why); ++refused; };
                if (it == got.end()) { said ("the probe returned nothing for index " + juce::String (w.first) + (callWhy.isNotEmpty() ? ": " + callWhy : juce::String())); continue; }
                if (it->second.name.trim() != n.trim()) { said ("the parameter at index " + juce::String (w.first) + " is '" + it->second.name + "' here"); continue; }
                std::vector<trust::Reading> rs; auto hold = w.second; for (double f : { 0.25, 0.5, 0.75 }) hold.push_back (f);
                int empty = 0;
                for (const auto& x : it->second.reads)
                {
                    if (x.text.trim().isEmpty()) { ++empty; continue; }
                    const double at = x.landed ? x.norm : x.taken; if (at < 0.0) continue;
                    if (std::any_of (hold.begin(), hold.end(), [&] (double h) { return std::abs (h - at) <= trust::HOLD_OUT_REACH + 1e-9; })) continue;   // a snap onto a held-out setting is not an anchor
                    rs.push_back ({ at, x.text, ! x.landed });
                }
                if (rs.empty() && empty > 0) { said ("the display is empty at every setting (" + juce::String (empty) + " reads): the plugin prints no text for it"); continue; }
                const auto rb = trust::anchorsFromReadings (rs);
                if (! rb.ok) { said (rb.why); continue; }
                auto* how = new juce::DynamicObject(); how->setProperty ("source", "anchor-repair"); how->setProperty ("grid_step", step);
                juce::Array<juce::var> hv; for (double x : w.second) hv.add (x); how->setProperty ("held_out", juce::var (hv));
                how->setProperty ("version", desc.version); how->setProperty ("measuredAt", rec->getProperty ("measuredAt"));
                map = repair::repairedMap (map, n, w.first, rb, juce::var (how));
                res->setProperty (n, rb.why); ++built;
                say ("REPAIR: " + desc.name + " / " + n + " - " + rb.why + (inMap.count (n) ? "" : " (added: the map lacked it)"));
            }
            rec->setProperty ("result", juce::var (res));
            opt.out.getChildFile ("repair").getChildFile (stem + ".repair.json").replaceWithText (juce::JSON::toString (juce::var (rec)));
            if (map.hasProperty ("repaired_controls")) opt.out.getChildFile ("maps").getChildFile (file).replaceWithText (juce::JSON::toString (map));
        }
    }
    say ("REPAIR: done - " + juce::String (built) + " control(s) rebuilt, " + juce::String (refused) + " not (said above); the map store was not written");
    return 0;
}

} // namespace ejmap::cert
