/*
  EjmapWizardResults.h - ONE RESULT PER PLUGIN (contract v1 §4, Kathy 21 Oct). The engine packs as it always has (the manifest and the
  bundle folder: the same states, the same files, the same personal-detail and category checks); this turns that pack into one
  ej_wizard_result/1 document per (kind, plugin) and puts each NEW one in the outbox, where the app sends it. A checkpoint emits only what
  is final; the run's last pack (finished / sent_early) emits the rest, "not reached" included. Nothing is emitted twice (the outbox keys
  on the result id), and a document that would carry anything personal is never written.
*/
#pragma once

#include "EjmapWizardContract.h"
#include <set>

namespace ejmap::wizresults
{
struct Doc { juce::String resultId, kind, pluginId; juce::var body; };
// the states a checkpoint may send: nothing that can still change
inline bool finalForCheckpoint (const juce::String& st)
{ return st.isNotEmpty() && st != "measuring" && st != "not_reached" && st != "queued" && st != "ready" && st != "pending_review"; }

struct Identity { juce::String vendor, product; };
inline juce::var pluginBlock (const juce::String& pid, const juce::String& product, const juce::String& vendor, const juce::var& fp)
{
    const auto parts = juce::StringArray::fromTokens (pid, "|", "");
    auto* p = new juce::DynamicObject(); p->setProperty ("plugin_id", pid); p->setProperty ("name", product); p->setProperty ("vendor", vendor);
    p->setProperty ("version", parts.size() >= 3 ? parts[2] : juce::String()); p->setProperty ("format", parts.size() >= 1 ? parts[0] : juce::String());
    const auto f = fp.toString(); p->setProperty ("fingerprint", f.isNotEmpty() ? juce::var (f) : juce::var());
    return juce::var (p);
}
inline juce::var fileJson (const juce::File& bundle, const juce::String& rel) { return juce::JSON::parse (bundle.getChildFile (rel).loadFileAsString()); }

// manifest + bundle folder -> the documents. idOf: plugin_id -> (vendor, product); pidOfProduct: a crash entry names only its product
// v1.1 (rulings 4 / 5): measuredAtOf - plugin_id -> when that plugin finished (the engine's record; REQUIRED on every result: a plugin the
// engine has no time for carries the packing time, never a later one); needIds - the server's queue, every plugin it answered "need" for
// this run: the run's last pack gives each one a result, "not_reached" when nothing else was sent for it
inline std::vector<Doc> fromPack (const juce::var& manifest, const juce::File& bundle, const juce::String& runId, bool finishing,
                                  const std::map<juce::String, Identity>& idOf, const std::map<juce::String, juce::String>& pidOfProduct,
                                  const std::map<juce::String, juce::String>& measuredAtOf = {}, const juce::StringArray& needIds = {})
{
    std::vector<Doc> out; const auto created = juce::Time::getCurrentTime().toISO8601 (true);
    auto add = [&] (const juce::String& kind, const juce::String& pid, const juce::String& product, const juce::var& fp, juce::DynamicObject* payload, juce::DynamicObject* files)
    {
        if (pid.isEmpty()) return;
        const auto it = idOf.find (pid);
        auto* d = new juce::DynamicObject(); const juce::var dv (d);
        d->setProperty ("schema", wizcontract::kResultSchema); d->setProperty ("run_id", runId);
        const auto rid = wizcontract::resultIdFor (runId, kind, pid); d->setProperty ("result_id", rid); d->setProperty ("kind", kind);
        d->setProperty ("plugin", pluginBlock (pid, product.isNotEmpty() ? product : (it != idOf.end() ? it->second.product : juce::String()), it != idOf.end() ? it->second.vendor : juce::String(), fp));
        d->setProperty ("created", created);   // the packing time
        { const auto m = measuredAtOf.find (pid); d->setProperty ("measured_at", m != measuredAtOf.end() && m->second.isNotEmpty() ? m->second : created); }   // v1.1: when it finished
        d->setProperty ("payload", payload != nullptr ? juce::var (payload) : juce::var (new juce::DynamicObject()));
        d->setProperty ("files", files != nullptr ? juce::var (files) : juce::var (new juce::DynamicObject()));
        out.push_back ({ rid, kind, pid, dv });
    };
    auto each = [&] (const char* list, const std::function<void (const juce::var&)>& fn) { if (const auto* a = manifest.getProperty (list, {}).getArray()) for (const auto& e : *a) fn (e); };

    each ("profiles", [&] (const juce::var& e)
    {
        auto* files = new juce::DynamicObject(); juce::StringArray rels;
        if (const auto* dr = e.getProperty ("drafts", {}).getArray()) for (const auto& r : *dr) rels.add (r.toString());
        for (const char* k : { "profile", "tone_check" }) if (e.hasProperty (k)) rels.add (e.getProperty (k, "").toString());
        if (e.hasProperty ("map_file")) rels.add (e.getProperty ("map_file", "").toString());   // 22 Oct: the full map, as map.json below
        for (const auto& rel : rels) if (rel.isNotEmpty()) files->setProperty (rel == e.getProperty ("map_file", "").toString() ? juce::String ("map.json") : juce::File::createLegalFileName (juce::File (rel).getFileName()), fileJson (bundle, rel));
        auto* pl = new juce::DynamicObject(); pl->setProperty ("category", e.getProperty ("kind", {})); pl->setProperty ("state", "measured");
        if (e.hasProperty ("acceptance")) pl->setProperty ("acceptance", e.getProperty ("acceptance", {}));
        add ("profile", e.getProperty ("plugin_id", "").toString(), e.getProperty ("product", "").toString(), e.getProperty ("map_fp", {}), pl, files);
    });
    each ("confirmations", [&] (const juce::var& e)
    {
        auto* pl = new juce::DynamicObject(); for (const char* k : { "profile_from_version", "readback", "matched_with_tolerance", "near_matches" }) if (e.hasProperty (k)) pl->setProperty (k, e.getProperty (k, {}));
        pl->setProperty ("state", "ready_from_version");
        add ("confirmation", e.getProperty ("plugin_id", "").toString(), e.getProperty ("product", "").toString(), {}, pl, nullptr);
    });
    each ("skipped", [&] (const juce::var& e)
    {
        const auto st = e.getProperty ("state", "").toString();
        if (! finishing && ! finalForCheckpoint (st)) return;   // a checkpoint: only what can no longer change
        if (! finishing && st == "not_measurable") return;       // a draft derived later may still make it measured
        auto* pl = new juce::DynamicObject(); pl->setProperty ("state", st); pl->setProperty ("why", e.getProperty ("why", {}));
        add ("outcome", e.getProperty ("plugin_id", "").toString(), e.getProperty ("product", "").toString(), {}, pl, nullptr);
    });
    each ("crashed", [&] (const juce::var& e)
    {
        const auto product = e.getProperty ("product", "").toString(); const auto it = pidOfProduct.find (product);
        auto* pl = new juce::DynamicObject(); pl->setProperty ("stage", e.getProperty ("stage", {})); pl->setProperty ("report_found", e.getProperty ("report_found", false));
        const auto rel = e.getProperty ("crash_report", "").toString(); if (rel.isNotEmpty()) pl->setProperty ("report", bundle.getChildFile (rel).loadFileAsString());
        add ("crash", it != pidOfProduct.end() ? it->second : juce::String(), product, {}, pl, nullptr);
    });
    auto withMap = [&] (const char* list, const char* kind)
    {
        each (list, [&] (const juce::var& e)
        {
            const auto entry = e.hasProperty ("file") ? fileJson (bundle, e.getProperty ("file", "").toString()) : e;
            auto* pl = new juce::DynamicObject();
            for (const char* k : { "leaning", "category", "signature", "why", "held_reason" }) if (entry.hasProperty (k)) pl->setProperty (k, entry.getProperty (k, {})); else if (e.hasProperty (k)) pl->setProperty (k, e.getProperty (k, {}));
            auto* files = new juce::DynamicObject(); if (entry.getProperty ("map", {}).isObject()) files->setProperty ("map.json", entry.getProperty ("map", {}));
            add (kind, e.getProperty ("plugin_id", "").toString(), e.getProperty ("product", "").toString(), e.getProperty ("map_fp", {}), pl, files);
        });
    };
    withMap ("needs_category", "needs_category"); withMap ("other_categories", "other_category"); withMap ("held", "held");
    each ("out_of_scope", [&] (const juce::var& e)
    { auto* pl = new juce::DynamicObject(); pl->setProperty ("why", e.getProperty ("why", {})); add ("out_of_scope", e.getProperty ("plugin_id", "").toString(), e.getProperty ("product", "").toString(), {}, pl, nullptr); });
    each ("reference_votes", [&] (const juce::var& e)
    {
        auto* pl = new juce::DynamicObject(); if (const auto* o = e.getDynamicObject()) for (const auto& [k, v] : o->getProperties()) pl->setProperty (k, v);
        add ("reference_vote", e.getProperty ("plugin_id", "").toString(), e.getProperty ("product", "").toString(), {}, pl, nullptr);
    });
    each ("bridge_reads", [&] (const juce::var& e)
    {
        auto* pl = new juce::DynamicObject(); pl->setProperty ("twin_plugin_id", e.getProperty ("twin_plugin_id", {}));
        auto* files = new juce::DynamicObject(); const auto rel = e.getProperty ("file", "").toString(); if (rel.isNotEmpty()) files->setProperty (juce::File (rel).getFileName(), fileJson (bundle, rel));
        add ("bridge_read", e.getProperty ("plugin_id", "").toString(), e.getProperty ("product", "").toString(), {}, pl, files);
    });
    if (finishing)   // v1.1 §5 (ruling 5): the server's queue - a plugin it answered "need" for that has no result yet gets its outcome
    {
        std::set<juce::String> have; for (const auto& d : out) have.insert (d.pluginId);
        for (const auto& pid : needIds)
            if (! have.count (pid))
            {   auto* pl = new juce::DynamicObject(); pl->setProperty ("state", "not_reached"); pl->setProperty ("why", "the run ended before this plugin was measured");
                add ("outcome", pid, {}, {}, pl, nullptr); }
    }
    return out;
}

// into the outbox: each document once; one that would carry anything personal is never written (the words say what)
struct Emitted { int added = 0, already = 0, refused = 0; juce::StringArray refusedWhy; };
inline Emitted emit (const std::vector<Doc>& docs, wizcontract::Outbox& box, const std::function<juce::StringArray (const juce::String&)>& personal)
{
    Emitted em;
    for (const auto& d : docs)
    {
        const auto text = juce::JSON::toString (d.body, true);
        if (personal) if (const auto p = personal (text); ! p.isEmpty()) { ++em.refused; em.refusedWhy.add (d.kind + " " + d.pluginId + ": " + p.joinIntoString (", ")); continue; }
        if (box.put (d.resultId, text)) ++em.added; else ++em.already;
    }
    return em;
}
} // namespace ejmap::wizresults
