/*
  EjmapWizardRun.h - the wizard's run (feat/ejmap-wizard): scan -> look up (A / B / C) -> the quick live check for B -> measure the
  queue (Phase B's "wizard" category: --jobs, resumable, every outcome and the tone check as in cert) -> the contribution bundle ->
  the upload (STUBBED: nothing is sent). The pure rules are in EjmapWizard.h.

    ejmap --wizard --db <profiles folder> --out <work folder> [--only NAME]... [--jobs N] [--measure] [--bundle]

  The work folder holds wizard_state.json (every plugin's state), wizard_queue.txt, phaseb/wizard/ (the measured rows) and the bundle.
*/
#pragma once

#include "EjmapWizard.h"
#include "EjmapCertDriver.h"
#include <unistd.h>

namespace ejmap::cert
{

struct WizardOptions { SweepOptions opt; juce::File db; juce::StringArray only; bool measure = false, bundle = false; };

// map_fp of an installed plugin from the local map store (read only): (uid, version) -> fp
inline std::map<juce::String, juce::String> localMapFps (const juce::File& ledger)
{
    std::map<juce::String, juce::String> m;
    for (const auto& f : ledger.getChildFile ("maps").findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        const auto d = juce::JSON::parse (f.loadFileAsString()); const auto id = d.getProperty ("identity", {});
        if (id.getProperty ("format", "").toString() != "AudioUnit") continue;
        m[id.getProperty ("uid", "").toString().toLowerCase() + "|" + id.getProperty ("version", "").toString()] = d.getProperty ("fp", "").toString();
    }
    return m;
}

// THE QUICK LIVE CHECK: the plugin's control list, then each written control read back at the profile's norms (one probe call each)
inline wizard::Readback quickCheck (const SweepOptions& opt, const juce::PluginDescription& desc, const juce::var& profile, bool& window)
{
    window = false;
    const juce::StringArray base { opt.probe.getFullPathName(), desc.name, desc.fileOrIdentifier, juce::String::toHexString (desc.uniqueId) };
    auto listArgs = base; listArgs.add ("--list-params");
    const auto lp = runChild (listArgs, opt.timeoutMs);
    if (lp.kind == ChildResult::Kind::uiShown) { window = true; return { false, 0, "a window appeared while reading its controls" }; }
    std::map<juce::String, int> names;
    for (const auto& line : juce::StringArray::fromLines (lp.out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() >= 2 && f[0].containsOnly ("0123456789") && f[0].isNotEmpty() && f[1].isNotEmpty() && ! names.count (f[1])) names[f[1]] = f[0].getIntValue();
    }
    const auto writes = wizard::writesOf (profile);
    std::map<int, juce::StringArray> norms;
    for (const auto& w : writes) { int idx = w.index; if (const auto it = names.find (w.control); it != names.end()) idx = it->second; if (idx >= 0) norms[idx].addIfNotAlreadyThere (wizard::normKey (w.norm)); }
    std::map<int, std::map<juce::String, juce::String>> observed;
    for (const auto& [idx, ns] : norms)
    {
        auto a = base; a.add ("--text-at-norms"); a.add (juce::String (idx)); a.add (ns.joinIntoString (","));
        const auto r = runChild (a, opt.timeoutMs);
        if (r.kind == ChildResult::Kind::uiShown) { window = true; return { false, 0, "a window appeared while reading its controls" }; }
        for (const auto& line : juce::StringArray::fromLines (r.out))
        {
            const auto f = juce::StringArray::fromTokens (line, "\t", "");
            if (f.size() < 4 || f[0] != "at") continue;
            const int t = f.indexOf ("text"); observed[idx][wizard::normKey (f[1].getDoubleValue())] = t >= 0 && t + 1 < f.size() ? f[t + 1] : juce::String();
        }
    }
    return wizard::readback (writes, names, observed);
}

inline int runWizard (const WizardOptions& wo)
{
    auto say = [] (const juce::String& s) { std::cout << s << std::endl; };
    const auto& opt = wo.opt; const auto out = opt.out; out.createDirectory();
    const auto db = wizard::loadDb (wo.db);
    say ("WIZARD: " + juce::String ((int) db.size()) + " profile(s) in the database (" + wo.db.getFileName() + ", a local folder: nothing is fetched)");
    // 1. SCAN + LOOK UP
    std::vector<InstalledRecord> plugins; for (const auto& r : installedAudioUnits()) if (wo.only.isEmpty() || wo.only.contains (r.desc.name)) plugins.push_back (r);
    const auto fps = localMapFps (opt.ledger);
    auto* state = new juce::DynamicObject(); juce::Array<juce::var> rows; juce::StringArray queue; std::map<juce::String, int> counts;
    for (const auto& ir : plugins)
    {
        const auto uid = juce::String::toHexString (ir.desc.uniqueId).toLowerCase(); const auto ver = ir.desc.version;
        const auto fp = fps.count (uid + "|" + ver) ? fps.at (uid + "|" + ver) : juce::String();
        const auto L = wizard::lookup (db, uid, ver, fp);
        auto* row = new juce::DynamicObject(); row->setProperty ("product", ir.desc.name); row->setProperty ("plugin_id", "AudioUnit|" + uid + "|" + ver); row->setProperty ("map_fp", fp);
        juce::String st, why = L.why;
        if (L.match == wizard::Match::exact) st = "ready";
        else if (L.match == wizard::Match::sameControls)
        {
            const auto gate = licenceGate (opt, ir.desc.name, ir.desc.manufacturerName, out, false);
            if (gate.stop.isNotEmpty()) { st = gate.stop.contains (uad::kNotConnected) ? "needs_hardware" : "needs_licence"; why = gate.stop; }
            else
            {
                bool window = false; const auto rb = quickCheck (opt, ir.desc, L.entry->profile, window);
                row->setProperty ("from_version", L.entry->version()); row->setProperty ("readback", rb.why);
                if (window) { st = "needs_licence"; why = rb.why; }
                else if (rb.pass) { st = "ready_from_version"; why = "the version " + L.entry->version() + " profile: " + rb.why; }
                else { st = "queued"; why = "the version " + L.entry->version() + " profile does not fit (" + rb.why + "): measure this version"; queue.add (ir.desc.name); }
            }
        }
        else { st = "queued"; queue.add (ir.desc.name); }
        row->setProperty ("state", st); row->setProperty ("why", why); rows.add (juce::var (row)); ++counts[st];
        say ("  " + ir.desc.name.paddedRight (' ', 30) + " " + ver.paddedRight (' ', 9) + " -> " + st + ": " + why);
    }
    out.getChildFile ("wizard_queue.txt").replaceWithText (queue.joinIntoString ("\n") + "\n");
    say ("WIZARD: " + juce::String ((int) plugins.size()) + " plugin(s) found: " + wizard::summaryLine (counts));
    // 2. MEASURE THE QUEUE (Phase B: --jobs, resumable, every outcome, the tone check)
    if (wo.measure && ! queue.isEmpty())
    {
        say ("WIZARD: measuring " + juce::String (queue.size()) + " plugin(s)" + (opt.jobs > 1 ? ", " + juce::String (opt.jobs) + " at a time" : juce::String()) + " - stop any time; it carries on next time");
        runPhaseBAll (opt, { "wizard" }, {}, {});
        counts.clear();
        for (auto& rv : rows)
        {
            auto* row = rv.getDynamicObject(); if (row->getProperty ("state").toString() != "queued") { ++counts[row->getProperty ("state").toString()]; continue; }
            juce::String outcome, certState, rowWhy;
            for (const auto& f : out.getChildFile ("phaseb/wizard").findChildFiles (juce::File::findFiles, false, "*.phaseb.json"))
            { const auto r = juce::JSON::parse (f.loadFileAsString()); if (r.getProperty ("product", "").toString() == row->getProperty ("product").toString()) { outcome = r.getProperty ("outcome", "").toString(); rowWhy = r.getProperty ("reason", "").toString();
                const auto oc = juce::JSON::parse (out.getChildFile ("phaseb/wizard/outcome").getChildFile (f.getFileName().replace (".phaseb.json", ".outcomes.json")).loadFileAsString());
                if (const auto* a = oc.getArray()) for (const auto& o : *a) { certState = o.getProperty ("state", "").toString(); if (rowWhy.isEmpty()) rowWhy = o.getProperty ("reason", "").toString(); } } }
            const auto st = outcome.isEmpty() ? juce::String ("measuring") : wizard::plainOutcome (outcome, certState);
            row->setProperty ("state", st); row->setProperty ("outcome", outcome); row->setProperty ("cert_state", certState); if (rowWhy.isNotEmpty()) row->setProperty ("why", rowWhy); ++counts[st];
        }
        say ("WIZARD: " + wizard::summaryLine (counts));
    }
    state->setProperty ("schema", "ej_wizard_state/0"); state->setProperty ("plugins", rows); state->setProperty ("summary", wizard::summaryLine (counts));
    out.getChildFile ("wizard_state.json").replaceWithText (juce::JSON::toString (juce::var (state)) + "\n");
    // 3. THE BUNDLE (profiles + tone-check sidecars + a manifest; nothing personal) and the STUBBED upload
    if (wo.bundle)
    {
        const auto B = out.getChildFile ("contribution"); B.deleteRecursively(); B.getChildFile ("profiles").createDirectory();
        juce::Array<juce::var> items, confirmations, skipped;
        for (const auto& rv : rows)
        {
            const auto st = rv.getProperty ("state", "").toString(); const auto product = rv.getProperty ("product", "").toString();
            if (st == "measured")
                for (const auto& f : out.getChildFile ("phaseb/wizard/profiles").findChildFiles (juce::File::findFiles, false, "*.json"))
                {
                    if (f.getFileName().endsWith (".tonecheck.json")) continue;
                    const auto p = juce::JSON::parse (f.loadFileAsString()); if (p.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString() != rv.getProperty ("plugin_id", "").toString()) continue;
                    const auto tc = f.getSiblingFile (f.getFileNameWithoutExtension() + ".tonecheck.json");
                    const auto tcv = juce::JSON::parse (tc.loadFileAsString());
                    if (! (bool) tcv.getProperty ("pass_within_0_5_db", false)) { auto* s = new juce::DynamicObject(); s->setProperty ("product", product); s->setProperty ("why", "measured, but its tone check did not pass: not sent"); skipped.add (juce::var (s)); continue; }
                    f.copyFileTo (B.getChildFile ("profiles").getChildFile (f.getFileName())); tc.copyFileTo (B.getChildFile ("profiles").getChildFile (tc.getFileName()));
                    auto* it = new juce::DynamicObject(); it->setProperty ("product", product); it->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); it->setProperty ("map_fp", p.getProperty ("plugin", {}).getProperty ("map_fp", {}));
                    it->setProperty ("profile", "profiles/" + f.getFileName()); it->setProperty ("tone_check", "profiles/" + tc.getFileName()); it->setProperty ("tone_check_g2_pass", true); items.add (juce::var (it));
                }
            else if (st == "ready_from_version")
            { auto* c = new juce::DynamicObject(); c->setProperty ("product", product); c->setProperty ("plugin_id", rv.getProperty ("plugin_id", {})); c->setProperty ("profile_from_version", rv.getProperty ("from_version", {})); c->setProperty ("readback", rv.getProperty ("readback", {})); confirmations.add (juce::var (c)); }
            else if (st != "ready")
            { auto* s = new juce::DynamicObject(); s->setProperty ("product", product); s->setProperty ("state", st); s->setProperty ("why", rv.getProperty ("why", {})); skipped.add (juce::var (s)); }
        }
        auto* M = new juce::DynamicObject(); M->setProperty ("schema", "ej_contribution/0"); M->setProperty ("created", juce::Time::getCurrentTime().toISO8601 (true));
        M->setProperty ("tool", juce::String ("EJ Map ") + EJMAP_GIT_HASH); M->setProperty ("platform", juce::SystemStats::getOperatingSystemName());
        M->setProperty ("profiles", items); M->setProperty ("confirmations", confirmations); M->setProperty ("skipped", skipped);
        M->setProperty ("note", "results only: profiles, their tone checks and this manifest - no plugin files, no audio, no account token");
        B.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (juce::var (M)) + "\n");
        // nothing personal: every file is read and checked before it is packed
        char host[256] = {}; gethostname (host, sizeof host - 1);
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(); const auto user = juce::SystemStats::getLogonName();
        juce::StringArray problems;
        for (const auto& f : B.findChildFiles (juce::File::findFiles, true))
            for (const auto& p : wizard::personalProblems (f.loadFileAsString(), home, user, juce::String (host).upToFirstOccurrenceOf (".", false, false)))
                problems.add (f.getRelativePathFrom (B) + ": " + p);
        if (! problems.isEmpty()) { say ("WIZARD: the bundle is NOT made - it would carry: " + problems.joinIntoString ("; ")); return 3; }
        const auto zip = out.getChildFile ("contribution.zip"); zip.deleteFile();
        juce::ChildProcess z; z.start (juce::StringArray { "/bin/sh", "-c", "cd \"" + B.getFullPathName() + "\" && /usr/bin/zip -qr \"" + zip.getFullPathName() + "\" ." }); z.waitForProcessToFinish (60000);
        say ("WIZARD: contribution bundle -> " + zip.getFileName() + " (" + juce::String (items.size()) + " new profile(s), " + juce::String (confirmations.size()) + " confirmation(s) of another version's profile, "
             + juce::String (skipped.size()) + " skipped with the reason; " + juce::String (zip.getSize() / 1024.0, 1) + " KB; checked: no paths, names or tokens)");
        say ("WIZARD: UPLOAD (stubbed): nothing is sent. A real app shows this list and sends only when the user presses Send.");
    }
    return 0;
}

} // namespace ejmap::cert
