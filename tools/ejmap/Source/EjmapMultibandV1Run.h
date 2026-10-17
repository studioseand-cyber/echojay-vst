/*
  EjmapMultibandV1Run.h - THE v1 MULTIBAND MEASURING (Kathy, 16 Oct 2026): what the Phase B multiband records do not hold, measured on top
  of them with the multiband mode's own probe calls (EjmapCertDriver.h runMultiband). Per multiband record in <cert>/phaseb/multiband/
  multiband/, one product at a time, licence gate first (a product the session knows needs a licence is not loaded):
    1. THE PER-BAND TONE CHECK: from the band's ladder, the threshold setting predicted for 2, 3 and 4 dB of gain reduction at -12 dBFS on
       the band's centre tone (mbv1::predictNorm); --sweep at {open end, that setting}, the same arguments as the ladder; GR read the
       ladder's way (deesser::ladderOf: against the open end)
    2. THE CROSSOVERS (Kathy, 16 Oct: no crossover type assumed): each crossover at the position the plugin opens with (v1 never moves one);
       the frequency where the two neighbouring bands' responses are EQUAL - each soloed in turn (a unit with per-band solos), or each
       compressed alone at its deep end with the others open, the cuts scaled by their own depth (no solo) - mbv1::crossingHz
    1b. THE THRESHOLD GRID: the ladder's positions over -54 .. -3 dBFS, so each position's onset can be read against its display
    3. THE MAKE-UP: a band's own gain / make-up / output control at 25 and 75 %, a 121-tone --response at -30 dBFS (no compression): the
       level step at the band's centre against the step its dB display shows
  Writes <cert>/phaseb/multiband/v1/<stem>.multiband_v1.json (ej_multiband_v1/1). Resumable: a product with its record is skipped. Stops
  between products at the run-all hour (EJ_RUNALL_UNTIL_MS) or on Ctrl-C / 07:00 (phasebStop).
    ejmap --cert-multiband-v1 --out <cert> [--only <product>]... [--probe <path>] [--ejmap-ledger <dir>]
*/

#pragma once

#include "EjmapCertDriver.h"
#include "EjmapMultibandV1.h"

namespace ejmap::cert
{

inline int runMultibandV1Product (const SweepOptions& opt, const juce::File& recordFile, const std::function<void (const juce::String&)>& say)
{
    using namespace multiband;
    const auto rec = juce::JSON::parse (recordFile.loadFileAsString());
    const auto product = rec.getProperty ("product", "").toString();
    std::vector<InstalledRecord> hits; for (const auto& r : installedAudioUnits()) if (r.desc.name == product) hits.push_back (r);
    if (hits.size() != 1) { say ("MBV1: '" + product + "' resolves to " + juce::String ((int) hits.size()) + " installed component(s): skipped"); return 2; }
    const auto& desc = hits[0].desc;
    if (const auto gate = licenceGate (opt, desc); gate.stop.isNotEmpty()) { say ("MBV1: " + product + " - " + gate.stop); return kToneLicenceKnownExit; }
    const auto uidHex = hits[0].uidKey.fromLastOccurrenceOf ("|", false, false); const auto stem = "AudioUnit_" + uidHex + "_" + desc.version;
    const auto mbDir = opt.out.getChildFile ("phaseb").getChildFile ("multiband");
    auto raw = mbDir.getChildFile ("v1raw"); raw.createDirectory(); auto outDir = mbDir.getChildFile ("v1"); outDir.createDirectory();
    int processN = 0; bool window = false;
    auto run = [&] (const juce::String& tag, const juce::StringArray& extra) {
        juce::StringArray args { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) }; args.addArray (extra);
        const auto r = runProbeCall (args, opt.timeoutMs); ++processN; if (r.kind == ChildResult::Kind::uiShown) window = true;
        raw.getChildFile (stem + ".mbv1." + tag + ".1.txt").replaceWithText (r.out, false, false, "\n"); return r; };
    Subject s; s.product = product; s.desc = desc; s.uid = uidHex; s.version = desc.version;
    const auto id = checkProbe (opt.probe, {}, {});
    const auto fx = sampledFixture (opt, desc, raw, stem, "multiband_v1", s, "signed EchoJayProbe, team " + id.team + ", cdhash " + id.cdhash,
                                    { "threshold", "thresh", "thr", "crossover", "xover", "cross", "x-over", "freq", "frequency", "hz", "amount", "depth", "compression", "gain", "makeup", "output", "level" });
    if (! fx.ok) { say ("MBV1: " + product + ": " + fx.why); return 1; }
    const auto N = nominateMultiband (fx.base, [] (const juce::String&) {});
    juce::StringArray enableSets;   // the record's enable step: written on every process, as the multiband mode did
    if (const auto* e = rec.getProperty ("enabled_by", {}).getArray()) for (const auto& x : *e) if (x.hasProperty ("index") && x.hasProperty ("norm")) enableSets.add (x.getProperty ("index", 0).toString() + ":" + juce::String ((double) x.getProperty ("norm", 0.0), 6));
    auto withEnables = [&] (juce::StringArray a, const juce::StringArray& sets) { juce::StringArray all = enableSets; all.addArray (sets); if (! all.isEmpty()) a.add ("set=" + all.joinIntoString (",")); return a; };
    std::map<juce::String, int> idxOf; std::map<int, juce::String> nameOf;
    if (const auto* cs = fx.base.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) { idxOf[c.getProperty ("name", "").toString()] = (int) c.getProperty ("index", -1); nameOf[(int) c.getProperty ("index", -1)] = c.getProperty ("name", "").toString(); }

    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", "ej_multiband_v1/1"); o->setProperty ("product", product); o->setProperty ("version", desc.version);
    o->setProperty ("plugin_id", "AudioUnit|" + uidHex + "|" + desc.version); o->setProperty ("from_record", recordFile.getFileName());
    struct LadderInfo { int band = 0, index = -1; double tone = 0.0, openNorm = 0.0, deepNorm = 1.0; std::vector<mbv1::Cell> cells; juce::String control; };
    std::map<int, LadderInfo> ladders;
    if (const auto* ls = rec.getProperty ("band_ladders", {}).getArray())
        for (const auto& l : *ls)
        {
            LadderInfo L; L.band = (int) l.getProperty ("band", 0); L.index = (int) l.getProperty ("index", -1); L.tone = (double) l.getProperty ("tone_hz", 0.0); L.control = l.getProperty ("control", "").toString();
            L.cells = mbv1::cellsOf (l);
            const auto at12 = mbv1::atLevel (L.cells, -12.0); double lo = 1e9, hi = -1e9;
            for (const auto& [n, g] : at12) { if (g < lo) { lo = g; L.openNorm = n; } if (g > hi) { hi = g; L.deepNorm = n; } }
            if (nameOf.count (L.index) && nameOf[L.index] != L.control) { say ("MBV1: " + product + ": control " + juce::String (L.index) + " is '" + nameOf[L.index] + "' here, the record's ladder says '" + L.control + "' - this band is not measured (another build)"); continue; }
            ladders[L.band] = L;
        }
    // 1 + 3. PER BAND: the tone check and the make-up
    juce::Array<juce::var> bandsOut;
    for (const auto& [band, L] : ladders)
    {
        auto* b = new juce::DynamicObject(); b->setProperty ("band", band); b->setProperty ("control", L.control); b->setProperty ("tone_hz", std::round (L.tone));
        juce::Array<juce::var> tc;
        for (double g : { 2.0, 3.0, 4.0 })
        {
            auto* t = new juce::DynamicObject(); t->setProperty ("asked_db", g);
            const auto n = mbv1::predictNorm (L.cells, -12.0, g);
            if (! n) { t->setProperty ("why", "the ladder does not reach it at -12 dBFS"); tc.add (juce::var (t)); continue; }
            t->setProperty ("norm", *n);
            const auto r = run ("b" + juce::String (band) + ".g" + juce::String ((int) g), withEnables ({ "--sweep", "thr=" + juce::String (L.index), "norms=" + juce::String (L.openNorm, 6) + "," + juce::String (*n, 6),
                                                                                 "levels=-12", "hz=" + juce::String (L.tone, 0), "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0" }, {}));
            if (window) break;
            const auto lad = deesser::ladderOf (sweep::parseSweep (r.cleanExit() ? r.out : juce::String()), L.tone);
            for (const auto& c : lad.cells) if (c.ok && std::abs (c.norm - (float) *n) < 1e-4) { t->setProperty ("gr_db", std::round (c.grDb * 100.0) / 100.0); t->setProperty ("display", c.text); }
            if (! t->hasProperty ("gr_db")) t->setProperty ("why", lad.ok ? juce::String ("no reading at the predicted setting") : lad.why + " (" + r.describe() + ")");
            tc.add (juce::var (t));
        }
        if (window) break;
        b->setProperty ("tonecheck", tc);
        {   // THE THRESHOLD GRID: the ladder's own six positions over a wider level grid (-54 .. -3 dBFS), so every position's onset falls inside
            juce::StringArray ns; { std::set<double> seen; for (const auto& c : L.cells) if (seen.insert (c.norm).second) ns.add (juce::String (c.norm, 6)); }
            const auto r = run ("b" + juce::String (band) + ".thr", withEnables ({ "--sweep", "thr=" + juce::String (L.index), "norms=" + ns.joinIntoString (","), "levels=-54,-48,-42,-36,-30,-24,-18,-12,-6,-3",
                                                                                  "hz=" + juce::String (L.tone, 0), "hold=1.50", "discard=0.75", "win=0.25", "ref=0", "moving_db=0.1", "reset=0" }, {}));
            if (window) break;
            const auto lad = deesser::ladderOf (sweep::parseSweep (r.cleanExit() ? r.out : juce::String()), L.tone);
            juce::Array<juce::var> cells;
            for (const auto& c : lad.cells) if (c.ok) { auto* co = new juce::DynamicObject(); co->setProperty ("norm", c.norm); co->setProperty ("display", c.text); co->setProperty ("level_dbfs", c.levelDbfs); co->setProperty ("gr_db", std::round (c.grDb * 100.0) / 100.0); cells.add (juce::var (co)); }
            b->setProperty ("threshold_cells", cells);
        }
        // the band's own gain / make-up / output: same band number, or the same band word as its threshold (Low / Mid / High...)
        const auto thrToks = nameTokens (L.control); const int thrBand = bandNumberOf (L.control);
        int gIdx = -1; juce::String gName;
        for (const auto& [nm, ix] : idxOf)
        {
            const auto toks = nameTokens (nm); bool gainWord = false, other = false;
            for (const auto& t : toks) { if (t == "gain" || t == "makeup" || t == "output" || t == "level" || t == "out") gainWord = true;
                                         if (t == "threshold" || t == "thresh" || t == "range" || t == "ratio" || t == "attack" || t == "release" || t == "knee" || t == "freq" || t == "frequency" || t == "crossover"
                                             || t == "q" || t == "sidechain" || t == "sc" || t == "solo" || t == "bypass" || t == "mute" || t == "on" || t == "input" || t == "in" || t == "dry" || t == "wet" || t == "mix") other = true; }
            if (! gainWord || other) continue;
            bool sameBand = thrBand > 0 ? bandNumberOf (nm) == thrBand : false;
            if (! sameBand && thrBand <= 0) for (const auto& w : { "low", "lo", "mid", "lomid", "himid", "high", "hi" }) if (thrToks.contains (w) && toks.contains (w)) sameBand = true;
            if (sameBand) { gIdx = ix; gName = nm; break; }
        }
        if (gIdx >= 0)
        {
            const auto r = run ("b" + juce::String (band) + ".makeup", withEnables ({ "--response", "ctl=" + juce::String (gIdx), "norms=0.25,0.75", "tones=121", "lo=20", "hi=20000", "db=-30", "hold=1.0", "discard=0.5" },
                                                                                  { juce::String (L.index) + ":" + juce::String (L.openNorm, 6) }));
            if (window) break;
            const auto resp = eq::parseResponse (r.cleanExit() ? r.out : juce::String());
            auto* m = new juce::DynamicObject(); m->setProperty ("control", gName);
            juce::Array<juce::var> pts;
            if (resp.ok && resp.positions.size() >= 2)
            {
                auto at = [&] (const eq::Position& p) { double best = 1e9, v = 0.0; for (const auto& t : p.tones) { const double d = std::abs (std::log2 (t.hz / L.tone)); if (d < best) { best = d; v = t.outDb - t.inDb; } } return v; };
                const auto d0 = mbv1::numberOf (resp.positions[0].text), d1 = mbv1::numberOf (resp.positions[1].text);
                auto* p = new juce::DynamicObject(); p->setProperty ("from", resp.positions[0].text); p->setProperty ("to", resp.positions[1].text);
                p->setProperty ("measured_delta_db", std::round ((at (resp.positions[1]) - at (resp.positions[0])) * 100.0) / 100.0);
                if (d0 && d1) p->setProperty ("display_delta_db", *d1 - *d0);
                pts.add (juce::var (p));
            }
            m->setProperty ("points", pts); b->setProperty ("makeup", juce::var (m));
        }
        bandsOut.add (juce::var (b));
    }
    o->setProperty ("bands", bandsOut);
    // 2. THE CROSSOVERS: sorted by their instantiate Hz, each between the band below and the band above (floating bands are not edges)
    juce::Array<juce::var> xo;
    if (! window && ! N.crossoverNames.isEmpty())
    {
        std::vector<std::pair<double, juce::String>> xs;
        for (int i = 0; i < N.crossoverNames.size(); ++i) { const auto hz = i < (int) N.crossoverDisplays.size() ? deesser::labelHz (N.crossoverDisplays[(size_t) i]) : std::nullopt; if (hz && idxOf.count (N.crossoverNames[i])) xs.push_back ({ *hz, N.crossoverNames[i] }); }
        std::sort (xs.begin(), xs.end());
        std::vector<Band> edgeBands; for (const auto& b : N.bands) { bool floating = false; for (const auto& [bn, ix] : N.floatingBandOf) if ((size_t) ix == (size_t) (b.index - 1)) floating = true; if (! floating) edgeBands.push_back (b); }
        std::sort (edgeBands.begin(), edgeBands.end(), [] (const Band& a, const Band& b) { return a.loHz < b.loHz; });
        for (size_t k = 0; k < xs.size() && k + 1 < edgeBands.size(); ++k)
        {
            const int below = edgeBands[k].index, above = edgeBands[k + 1].index;
            auto* x = new juce::DynamicObject(); x->setProperty ("control", xs[k].second); x->setProperty ("band_below", below); x->setProperty ("band_above", above);
            if (! ladders.count (below)) { x->setProperty ("why", "the band below has no ladder: not measured"); xo.add (juce::var (x)); continue; }
            if (! ladders.count (above)) { x->setProperty ("why", "the band above has no ladder: not measured"); xo.add (juce::var (x)); continue; }
            const auto& Lb = ladders[below]; const auto& La = ladders[above]; const int xi = idxOf[xs[k].second];
            juce::StringArray allOpen; for (const auto& [bn, l] : ladders) allOpen.add (juce::String (l.index) + ":" + juce::String (l.openNorm, 6));   // every threshold open
            juce::StringArray base { "--response", "ctl=" + juce::String (xi), "norms=current", "tones=121", "lo=20", "hi=20000", "db=-12", "hold=1.0", "discard=0.5" };
            auto respOf = [&] (const juce::String& tag, const juce::StringArray& sets) { const auto r = run ("x" + juce::String (k) + "." + tag, withEnables (base, sets)); return eq::parseResponse (r.cleanExit() ? r.out : juce::String()); };
            auto levels = [] (const eq::Position& p) { std::vector<std::pair<double, double>> v; for (const auto& t : p.tones) v.push_back ({ t.hz, t.outDb - t.inDb }); return v; };
            // the band's solo: a two-state control named solo with the band's number or word; its "Solo" / "On" position
            auto soloOf = [&] (int band, const juce::String& thrName) -> juce::String {
                const auto thrToks = nameTokens (thrName); const int bn = bandNumberOf (thrName);
                if (const auto* cs = fx.base.getProperty ("controls", {}).getArray())
                    for (const auto& c : *cs)
                    {
                        const auto nm = c.getProperty ("name", "").toString(); const auto toks = nameTokens (nm); if (! toks.contains ("solo")) continue;
                        bool same = bn > 0 ? bandNumberOf (nm) == bn : false;
                        if (! same && bn <= 0) for (const auto& w : { "low", "lo", "mid", "high", "hi" }) if (thrToks.contains (w) && toks.contains (w)) same = true;
                        if (! same) continue;
                        double on = 1.0; if (const auto* da = c.getProperty ("displayAt", {}).getDynamicObject()) for (const auto& p : da->getProperties()) { const auto t = p.value.toString().trim(); if (t.equalsIgnoreCase ("solo") || t.equalsIgnoreCase ("on")) on = p.name.toString().getDoubleValue(); }
                        return juce::String ((int) c.getProperty ("index", -1)) + ":" + juce::String (on, 6);
                    }
                juce::ignoreUnused (band); return {}; };
            // EJ_MBV1_ISOLATED=1 (a test switch, 16 Oct): take the isolated path on a unit that has solos, to check the two agree
            const bool forceIsolated = juce::String (std::getenv ("EJ_MBV1_ISOLATED") != nullptr ? std::getenv ("EJ_MBV1_ISOLATED") : "") == "1";
            const auto soloB = forceIsolated ? juce::String() : soloOf (below, Lb.control), soloA = forceIsolated ? juce::String() : soloOf (above, La.control);
            juce::Array<juce::var> ps; auto* p = new juce::DynamicObject();
            std::optional<double> hz; juce::String method, display;
            if (soloB.isNotEmpty() && soloA.isNotEmpty())
            {
                juce::StringArray sb = allOpen; sb.add (soloB); juce::StringArray sa = allOpen; sa.add (soloA);
                const auto rb = respOf ("solo_below", sb); if (window) break;
                const auto ra = respOf ("solo_above", sa); if (window) break;
                method = "solo: each neighbour soloed, every threshold open";
                if (rb.ok && ra.ok && ! rb.positions.empty() && ! ra.positions.empty()) { display = rb.positions[0].text; hz = mbv1::crossingHz (levels (rb.positions[0]), levels (ra.positions[0]), Lb.tone, La.tone); }
            }
            else
            {
                auto deepSets = [&] (const LadderInfo& L) { juce::StringArray s2; for (const auto& [bn, l] : ladders) s2.add (juce::String (l.index) + ":" + juce::String (l.index == L.index ? l.deepNorm : l.openNorm, 6)); return s2; };
                const auto ro = respOf ("open", allOpen); if (window) break;
                const auto rdb = respOf ("deep_below", deepSets (Lb)); if (window) break;
                const auto rda = respOf ("deep_above", deepSets (La)); if (window) break;
                method = "isolated: each neighbour compressed alone at its deep end, the others open; cuts scaled by their own depth";
                if (ro.ok && rdb.ok && rda.ok && ! ro.positions.empty() && ! rdb.positions.empty() && ! rda.positions.empty())
                {
                    display = ro.positions[0].text;
                    auto cb = eq::deviation (rdb.positions[0], ro.positions[0]), ca = eq::deviation (rda.positions[0], ro.positions[0]);
                    std::sort (cb.begin(), cb.end()); std::sort (ca.begin(), ca.end());
                    hz = mbv1::crossingHz (mbv1::normalisedCut (cb), mbv1::normalisedCut (ca), Lb.tone, La.tone);
                }
            }
            x->setProperty ("method", method);
            p->setProperty ("display", display);
            if (hz) p->setProperty ("measured_hz", std::round (*hz)); else p->setProperty ("why", "the two bands' responses do not cross between their centres");
            ps.add (juce::var (p));
            x->setProperty ("positions", ps); xo.add (juce::var (x));
        }
    }
    o->setProperty ("crossovers", xo);
    if (window) { say ("MBV1: " + product + ": a window appeared - stopped, nothing written"); return 5; }
    o->setProperty ("processes", processN); o->setProperty ("measuredAt", nowStamp());
    o->setProperty ("method", "per band: --sweep at {open end, the ladder's predicted setting for 2 / 3 / 4 dB at -12 dBFS} on the band's centre tone; crossovers: at the opening position, the frequency where the two neighbouring bands' responses are equal (each soloed, or each compressed alone at its deep end with the cuts scaled by their own depth), 121-tone --response; make-up: --response at -30 dBFS, the level step at the band's centre vs the display step");
    outDir.getChildFile (stem + ".multiband_v1.json").replaceWithText (juce::JSON::toString (juce::var (o)) + "\n", false, false, "\n");
    say ("MBV1: " + product + " -> " + juce::String (bandsOut.size()) + " band(s), " + juce::String (xo.size()) + " crossover(s), " + juce::String (processN) + " processes");
    return 0;
}

// THE BATCH: every multiband record in <cert>, resumable, stops between products at the run-all hour or on a stop
inline int runMultibandV1All (const SweepOptions& opt, const juce::StringArray& only)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto id = checkProbe (opt.probe, {}, {}); if (! id.ok) { say ("MBV1: ABORTED BEFORE ANY PLUGIN - " + id.why); return 3; }
    const auto recDir = opt.out.getChildFile ("phaseb").getChildFile ("multiband").getChildFile ("multiband");
    const auto outDir = opt.out.getChildFile ("phaseb").getChildFile ("multiband").getChildFile ("v1");
    const auto records = recDir.findChildFiles (juce::File::findFiles, false, "*.multiband.json");
    say ("MBV1: " + juce::String (records.size()) + " multiband record(s) in " + recDir.getFullPathName());
    const auto untilMs = juce::String (std::getenv ("EJ_RUNALL_UNTIL_MS") != nullptr ? std::getenv ("EJ_RUNALL_UNTIL_MS") : "").getLargeIntValue();
    int done = 0, skipped = 0, refused = 0;
    for (const auto& f : records)
    {
        if (phasebStop() != 0) { say ("MBV1: stopped - the rest run on the resume"); return 130; }
        if (untilMs > 0 && juce::Time::currentTimeMillis() >= untilMs) { say ("MBV1: the run's hour reached - the rest run on the resume"); return 130; }
        const auto product = juce::JSON::parse (f.loadFileAsString()).getProperty ("product", "").toString();
        if (! only.isEmpty() && ! only.contains (product)) continue;
        if (outDir.getChildFile (f.getFileNameWithoutExtension().upToLastOccurrenceOf (".multiband", false, false) + ".multiband_v1.json").existsAsFile()) { ++skipped; continue; }   // resumable
        const int rc = runMultibandV1Product (opt, f, say);
        if (rc == 0) ++done; else ++refused;
        if (rc == 5) return 5;   // a window: stop, as every cert mode does
    }
    say ("MBV1: done - " + juce::String (done) + " measured, " + juce::String (skipped) + " already measured, " + juce::String (refused) + " not measured (said above)");
    return 0;
}

} // namespace ejmap::cert
