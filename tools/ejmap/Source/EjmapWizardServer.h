/*
  EjmapWizardServer.h - THE SERVER BOUNDARY (Sean's review, Kathy 12 Oct; feat/ejmap-wizard; docs/WIZARD_SPEC.md section 6).

  1. THE BASE URL: https://www.echojay.ai (not api.echojay.ai); every endpoint under /api/wizard/v1/.
  2. THE UPLOAD PATH: a bundle that fits Vercel's 4.5 MB request body goes direct (POST /contributions, multipart); a bigger one goes
     through signed blob storage: POST /contributions/upload-url -> PUT the zip to the signed URL -> POST /contributions/complete with the
     blob reference and the manifest.
  3. THE CATEGORY TABLE: EJ Map keeps its own internal names; what the wizard SENDS (and what /lookup returns) uses the server's 12, in
     Sean's spellings. One table, here; an outgoing manifest whose categories are not all among the 12 is refused before packing.
  4. THE STUBS (the server is Sean's): sign-in, upload (both paths) and promo, writing under <work>/server_stub/. Nothing leaves the Mac.
     SERVER-SIDE, awaiting Sean: the account -> mapper-token bridge, the review / holding area, the promo rules + issuing + email.

  Pinned in RoundTripTest.cpp (WZ-SRV-*).
*/
#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace ejmap::wizserver
{
// --- 1. the base URL -------------------------------------------------------------------------------------------------------------------
inline juce::String baseUrl() { return "https://www.echojay.ai"; }
inline juce::String endpoint (const juce::String& path) { return baseUrl() + "/api/wizard/v1/" + path; }

// --- 2. the upload path --------------------------------------------------------------------------------------------------------------
// 4.5 MB as Vercel states it (decimal); the multipart body is the zip + the manifest + headers, so 64 KB of headroom is kept
inline constexpr juce::int64 kDirectBodyMaxBytes = 4500000, kMultipartHeadroomBytes = 64 * 1024;
enum class UploadPath { direct, signedBlob };
inline UploadPath uploadPathFor (juce::int64 bundleBytes, juce::int64 manifestBytes)
{ return bundleBytes + manifestBytes + kMultipartHeadroomBytes <= kDirectBodyMaxBytes ? UploadPath::direct : UploadPath::signedBlob; }

// --- 3. the category table -------------------------------------------------------------------------------------------------------------
inline const juce::StringArray& serverCategories()
{
    static const juce::StringArray k { "compressor", "limiter", "eq", "de-esser", "delay", "reverb", "saturation", "gate", "transient_shaper", "channel_strip", "amp_sim", "pitch" };
    return k;
}
// internal (EJ Map / the wizard) -> the server's name; "" = an internal label, never sent as a category (multiband, modulation, meter,
// utility, instrument, midi, the signature's "dynamics"). A saturation row that read an amp sim's cabinet response is amp_sim.
inline juce::String toServer (const juce::String& internal, bool cabinetRead = false)
{
    const auto c = internal.toLowerCase();
    if (c == "compressor" || c == "wizard") return "compressor";
    if (c == "limiter" || c == "limitercomp") return "limiter";
    if (c == "eq" || c == "wizard_eq") return "eq";
    if (c == "deesser" || c == "de-esser") return "de-esser";
    if (c == "delay") return "delay";
    if (c == "reverb") return "reverb";
    if (c == "saturation") return cabinetRead ? "amp_sim" : "saturation";
    if (c == "amp_sim") return "amp_sim";
    if (c == "gate") return "gate";
    if (c == "transient" || c == "transient_shaper") return "transient_shaper";
    if (c == "strips" || c == "strip" || c == "channel_strip") return "channel_strip";
    if (c == "tuners" || c == "tuner" || c == "pitch") return "pitch";
    return {};
}
inline bool isServerCategory (const juce::String& c) { return serverCategories().contains (c); }
// every category an outgoing manifest carries must be one of the 12: profiles[].kind, other_categories[].category,
// needs_category[].leaning (when not null); held[] and out_of_scope[] carry none
inline juce::StringArray invalidOutgoing (const juce::var& manifest)
{
    juce::StringArray bad;
    auto each = [&] (const char* list, const char* field)
    {
        if (const auto* a = manifest.getProperty (list, {}).getArray())
            for (const auto& e : *a)
            {
                const auto v = e.getProperty (field, {});
                if (v.isVoid() || (juce::String (field) == "leaning" && v.toString().isEmpty())) continue;
                if (! isServerCategory (v.toString())) bad.add (juce::String (list) + "." + field + " '" + v.toString() + "' (" + e.getProperty ("product", "").toString() + ")");
            }
    };
    each ("profiles", "kind"); each ("other_categories", "category"); each ("needs_category", "leaning");
    for (const char* list : { "held", "out_of_scope" })
        if (const auto* a = manifest.getProperty (list, {}).getArray())
            for (const auto& e : *a) if (e.hasProperty ("category") || e.hasProperty ("kind")) bad.add (juce::String (list) + " carries a category (" + e.getProperty ("product", "").toString() + ")");
    return bad;
}

// THE DEVICE HASH (items 7 / 8): what the app sends to identify a Mac without identifying it - SHA-256 of a fixed app salt and the Mac's
// hardware UUID (IOPlatformUUID). The raw UUID never leaves the Mac; the salt makes the hash useless to anyone matching other apps' IDs.
inline juce::String deviceHashOf (const juce::String& platformUuid)
{
    if (platformUuid.trim().isEmpty()) return {};
    return juce::SHA256 (("echojay-wizard-device-v1|" + platformUuid.trim().toUpperCase()).toUTF8()).toHexString();
}
// --- 4. the stubs (Sean's endpoints; nothing leaves the Mac) -----------------------------------------------------------------------------
namespace stub
{
inline juce::var signIn (const juce::File& work, const juce::String& email, const juce::String& password)
{   // POST /sign-in {email, password} -> {account_id, display_name, token, expires}   [SERVER-SIDE: the account -> mapper-token bridge]
    if (! email.contains ("@") || password.isEmpty()) return {};
    auto* o = new juce::DynamicObject(); o->setProperty ("account_id", "acct_stub_" + juce::SHA256 (email.toLowerCase().toUTF8()).toHexString().substring (0, 10));
    o->setProperty ("email", email); o->setProperty ("display_name", email.upToFirstOccurrenceOf ("@", false, false)); o->setProperty ("stub", true);
    o->setProperty ("endpoint", endpoint ("sign-in"));
    const juce::var v (o);   // ONE owner
    work.getChildFile ("account.json").replaceWithText (juce::JSON::toString (v));
    return v;
}
// THE REFERENCE VOTES (Kathy, 12 Oct), stubbed: the server records every vote (server_stub/reference_votes.json, appended) and keeps a
// tally per reference (server_stub/references/<file>: agreeing = the count it knew + each agreeing vote; a disagreeing vote's opening
// block kept for a third source). A reference that reaches 2 agreeing Macs is returned as now agreed (trusted).   [SERVER-SIDE: Sean]
inline juce::StringArray recordVotes (const juce::File& work, const juce::var& m)
{
    juce::StringArray nowAgreed;
    const auto votes = m.getProperty ("reference_votes", {}); const auto* a = votes.getArray();
    if (a == nullptr || a->isEmpty()) return nowAgreed;
    const auto dir = work.getChildFile ("server_stub/references"); dir.createDirectory();
    const auto logF = work.getChildFile ("server_stub/reference_votes.json"); auto log = juce::JSON::parse (logF.loadFileAsString());
    juce::Array<juce::var> logA; if (const auto* la = log.getArray()) logA = *la;
    for (const auto& v : *a)
    {
        const auto ref = juce::File::createLegalFileName (v.getProperty ("reference", "").toString()); if (ref.isEmpty()) continue;
        logA.add (v);
        const auto tf = dir.getChildFile (ref); auto t = juce::JSON::parse (tf.loadFileAsString());
        if (! t.isObject()) { auto* o = new juce::DynamicObject(); o->setProperty ("reference", ref); o->setProperty ("agreeing", v.getProperty ("agreeing_before", 0)); o->setProperty ("disagreeing", juce::Array<juce::var>()); t = juce::var (o); }
        auto* o = t.getDynamicObject(); const int before = (int) o->getProperty ("agreeing");
        if ((bool) v.getProperty ("agrees", false)) { o->setProperty ("agreeing", before + 1); if (before < 2 && before + 1 >= 2) nowAgreed.addIfNotAlreadyThere (ref); }
        else { auto d = o->getProperty ("disagreeing"); juce::Array<juce::var> da; if (const auto* x = d.getArray()) da = *x;
               auto* e = new juce::DynamicObject(); e->setProperty ("product", v.getProperty ("product", {})); e->setProperty ("opening_block", v.getProperty ("opening_block", {})); da.add (juce::var (e));
               o->setProperty ("disagreeing", da); }
        o->setProperty ("trusted", (int) o->getProperty ("agreeing") >= 2);
        tf.replaceWithText (juce::JSON::toString (t));
    }
    logF.replaceWithText (juce::JSON::toString (juce::var (logA)));
    return nowAgreed;
}
// THE MOCK DB'S AGREED REFERENCES (Kathy, 13 Oct: "seed the 290 agreed references into the wizard's mock DB only"): the opening
// blocks Sean's Mac and Kathy's agreed on at one version (the 12 Oct census; 24 Melda with different parameter counts carry the
// controls both show). The app ships them in its Resources (wizard_app/mock_db/references) and copies each into the stub DB's
// references/ when that file is not there yet - never over one, which a vote may have moved on. Never in a cert folder.
struct Seeded { int copied = 0, kept = 0; };
inline Seeded seedMockReferences (const juce::File& bundled, const juce::File& db)
{
    Seeded s; if (! bundled.isDirectory()) return s;
    const auto dst = db.getChildFile ("references"); dst.createDirectory();
    for (const auto& f : bundled.findChildFiles (juce::File::findFiles, false, "*.reference.json"))
    {
        const auto t = dst.getChildFile (f.getFileName());
        if (t.existsAsFile()) { ++s.kept; continue; }
        if (f.copyFileTo (t)) ++s.copied;
    }
    return s;
}
// --- ELIGIBILITY AND ONCE ONLY (Kathy, 13 Oct, beta items 7 and 8) - the SERVER decides; the app only shows it --------------------------
// config (server-configurable): reward_minimum (default 5) and once_only (default true: one completed contribution per account AND per
// Mac; false would allow repeat contributions, and a repeat never earns a second reward). Stub: <work>/server_stub/config.json.
inline constexpr int kQualifyMinimumDefault = 25;
inline juce::var serverConfig (const juce::File& work)
{
    auto c = juce::JSON::parse (work.getChildFile ("server_stub/config.json").loadFileAsString());
    auto* o = new juce::DynamicObject(); const juce::var v (o);
    // 13 Oct (Kathy): qualify_minimum (default 25) - at scan time the eligible new plugins must reach it, and the finished run's accepted
    // results must still reach it (the safety floor). An older config's reward_minimum is read as the same number
    const int q = c.isObject() && c.hasProperty ("qualify_minimum") ? (int) c.getProperty ("qualify_minimum", kQualifyMinimumDefault)
                : c.isObject() && c.hasProperty ("reward_minimum") ? (int) c.getProperty ("reward_minimum", kQualifyMinimumDefault) : kQualifyMinimumDefault;
    o->setProperty ("qualify_minimum", q); o->setProperty ("reward_minimum", q);
    o->setProperty ("once_only", c.isObject() && c.hasProperty ("once_only") ? (bool) c.getProperty ("once_only", true) : true);
    return v;
}
// who has taken part: an account or a Mac (its device hash) with an ACCEPTED upload - <work>/server_stub/participation.json
inline juce::var participation (const juce::File& work)
{
    auto p = juce::JSON::parse (work.getChildFile ("server_stub/participation.json").loadFileAsString());
    if (! p.isObject()) { auto* o = new juce::DynamicObject(); o->setProperty ("accounts", juce::Array<juce::var>()); o->setProperty ("devices", juce::Array<juce::var>()); p = juce::var (o); }
    return p;
}
inline bool listed (const juce::var& arr, const juce::String& x) { if (x.isEmpty()) return false; if (const auto* a = arr.getArray()) for (const auto& e : *a) if (e.toString() == x) return true; return false; }
inline bool alreadyContributed (const juce::File& work, const juce::String& accountId, const juce::String& device)
{
    if (! (bool) serverConfig (work).getProperty ("once_only", true)) return false;
    const auto p = participation (work);
    return listed (p.getProperty ("accounts", {}), accountId) || listed (p.getProperty ("devices", {}), device);
}
// /lookup's eligibility: NEW CONTRIBUTABLE plugins = rule C (to be measured: state "queued" - not pending_review, held, out of scope,
// licence-blocked or a demo); eligible when the count reaches the minimum and neither the account nor the Mac has taken part
inline juce::var lookupEligibility (const juce::File& work, const juce::String& accountId, const juce::String& device, const juce::var& wizardState)
{
    int n = 0; if (const auto* a = wizardState.getProperty ("plugins", {}).getArray()) for (const auto& r : *a) if (r.getProperty ("state", "").toString() == "queued") ++n;
    const int minimum = (int) serverConfig (work).getProperty ("qualify_minimum", kQualifyMinimumDefault);
    const bool already = alreadyContributed (work, accountId, device);
    auto* o = new juce::DynamicObject(); const juce::var v (o);
    o->setProperty ("already_contributed", already); o->setProperty ("count", n); o->setProperty ("minimum", minimum); o->setProperty ("eligible", ! already && n >= minimum);
    o->setProperty ("endpoint", endpoint ("lookup"));
    return v;
}
// the accepted count, from the BUNDLE itself (never the client's count): each profile item whose evidence is in the zip - a compressor's
// tone check passed within 0.5 dB, an EQ's acceptance, a draft that is there - counted once per plugin_id, and not if the server already
// knows the plugin (its database, or an earlier accepted upload)
inline int acceptedNew (const juce::File& work, const juce::File& storedZip, const juce::var& m, juce::StringArray& acceptedIds)
{
    juce::ZipFile z (storedZip);
    auto entryText = [&] (const juce::String& name) -> juce::String {
        for (int i = 0; i < z.getNumEntries(); ++i) if (const auto* e = z.getEntry (i); e != nullptr && e->filename.endsWith (name))
        { std::unique_ptr<juce::InputStream> in (z.createStreamForEntry (i)); return in ? in->readEntireStreamAsString() : juce::String(); }
        return {}; };
    juce::StringArray known; for (const auto& f : work.getChildFile ("server_stub/db").findChildFiles (juce::File::findFiles, false, "*.json"))
        if (const auto p = juce::JSON::parse (f.loadFileAsString()); p.isObject()) known.add (p.getProperty ("plugin", {}).getProperty ("plugin_id", "").toString());
    const auto acceptedBefore = juce::JSON::parse (work.getChildFile ("server_stub/accepted_plugins.json").loadFileAsString());   // held: getArray() on a temporary dangles
    if (const auto* a = acceptedBefore.getArray()) for (const auto& x : *a) known.add (x.toString());
    if (const auto* items = m.getProperty ("profiles", {}).getArray())
        for (const auto& it : *items)
        {
            const auto id = it.getProperty ("plugin_id", "").toString();
            if (id.isEmpty() || known.contains (id) || acceptedIds.contains (id)) continue;
            bool ok = false;
            if (it.hasProperty ("tone_check")) ok = (bool) juce::JSON::parse (entryText (it.getProperty ("tone_check", "").toString())).getProperty ("pass_within_0_5_db", false);
            else if (it.hasProperty ("acceptance")) ok = entryText (it.getProperty ("profile", "").toString()).isNotEmpty() && it.getProperty ("acceptance", "").toString().isNotEmpty();
            else if (const auto* ds = it.getProperty ("drafts", {}).getArray()) { ok = ! ds->isEmpty(); for (const auto& d : *ds) if (entryText (d.toString()).isEmpty()) ok = false; }
            if (ok) acceptedIds.add (id);
        }
    return acceptedIds.size();
}
// --- THE RUN (Kathy, 13 Oct: partial uploads) - [SERVER-SIDE: Sean] -----------------------------------------------------------------------
// Every upload carries run {run_id, kind: checkpoint | sent_early | finished}, the run's queue and each queued plugin's state. The server
// keeps ONE merged record per run id: plugin_id -> {state, accepted}. Merging is idempotent (the same upload twice changes nothing) and
// never double-counts: a plugin is accepted once per run, a final state is never replaced by a non-final one, measured beats any other.
// Pausing across days is the same run (the same run id). Only a FINISHED upload is judged for the code, on the merged run.
inline bool isFinalState (const juce::String& st)
{   // a final outcome: measured, or skipped for licence / hardware / crash / demo / silent / timeout / won't load / not measurable / custom default
    static const juce::StringArray k { "measured", "not_measurable", "needs_licence", "needs_hardware", "crashed", "demo", "silent", "wont_load", "custom_default", "timed_out" };
    return k.contains (st);
}
inline juce::var newRun (const juce::String& runId)
{ auto* o = new juce::DynamicObject(); o->setProperty ("run_id", runId); o->setProperty ("plugins", juce::var (new juce::DynamicObject())); o->setProperty ("queue", juce::Array<juce::var>()); o->setProperty ("uploads", 0); return juce::var (o); }
// merge one upload's manifest; acceptedNow: the plugin_ids whose evidence in THIS upload's zip passed (acceptedNew)
inline void mergeRun (juce::var& run, const juce::var& m, const juce::StringArray& acceptedNow)
{
    auto* plugins = run.getProperty ("plugins", {}).getDynamicObject();
    auto queue = run.getProperty ("queue", {}); juce::StringArray q; if (const auto* a = queue.getArray()) for (const auto& x : *a) q.add (x.toString());
    if (const auto* a = m.getProperty ("queue", {}).getArray()) for (const auto& x : *a) q.addIfNotAlreadyThere (x.toString());
    juce::Array<juce::var> qa; for (const auto& x : q) qa.add (x); run.getDynamicObject()->setProperty ("queue", qa);
    auto put = [&] (const juce::String& pid, const juce::String& st, bool accepted)
    {
        if (pid.isEmpty()) return;
        const juce::Identifier id (pid); auto cur = plugins->getProperty (id);
        if (! cur.isObject()) { auto* o = new juce::DynamicObject(); o->setProperty ("state", ""); o->setProperty ("accepted", false); cur = juce::var (o); plugins->setProperty (id, cur); }
        auto* c = cur.getDynamicObject(); const auto was = c->getProperty ("state").toString();
        // measured is never replaced; a final state replaces a non-final one (or a later final); a non-final state only fills a gap
        if (was != "measured" && (st == "measured" || isFinalState (st) || (! isFinalState (was) && st.isNotEmpty()))) c->setProperty ("state", st);
        if (accepted) c->setProperty ("accepted", true);
    };
    if (const auto* a = m.getProperty ("outcomes", {}).getArray()) for (const auto& o : *a) put (o.getProperty ("plugin_id", "").toString(), o.getProperty ("state", "").toString(), false);
    for (const auto& pid : acceptedNow) put (pid, "measured", true);
    run.getDynamicObject()->setProperty ("uploads", (int) run.getProperty ("uploads", 0) + 1);
}
struct RunTally { int queued = 0, final_ = 0, accepted = 0; };
inline RunTally tallyRun (const juce::var& run)
{
    RunTally t; const auto plugins = run.getProperty ("plugins", {});
    if (const auto* a = run.getProperty ("queue", {}).getArray())
        for (const auto& x : *a) { ++t.queued; if (isFinalState (plugins.getProperty (juce::Identifier (x.toString()), {}).getProperty ("state", "").toString())) ++t.final_; }
    if (auto* d = plugins.getDynamicObject()) for (const auto& nv : d->getProperties()) if ((bool) nv.value.getProperty ("accepted", false)) ++t.accepted;
    return t;
}
// THE REWARD (Kathy, 13 Oct): a code only for a FINISHED upload whose merged run is complete (every queued plugin has a final outcome) and
// whose accepted results still reach the qualify minimum, for an account and a Mac that have not taken part before. Words for every "no".
struct RewardDecision { bool eligible = false; juce::String message; };
inline RewardDecision rewardFor (const juce::String& kind, const RunTally& t, int minimum, bool before)
{
    if (before) return { false, "Thank you - this account or this Mac has already taken part, so there is no new code." };
    if (kind == "checkpoint") return { false, {} };   // a partial upload never earns on its own (and says nothing)
    if (kind == "sent_early") return { false, "Thank you - your results arrived. The free month needs the full run, so there's no code this time." };
    if (kind != "finished") return { false, "Thank you - your results arrived." };
    if (t.final_ < t.queued) return { false, "Thank you - your results arrived. The free month needs the full run: " + juce::String (t.final_) + " of " + juce::String (t.queued) + " plugins have a result so far." };
    if (t.accepted < minimum) return { false, "Thank you - your results arrived. The free month needs at least " + juce::String (minimum) + " new plugins that pass our checks; this time there " + (t.accepted == 1 ? juce::String ("was 1.") : "were " + juce::String (t.accepted) + ".") };
    return { true, {} };
}
// the server's genuine check, stubbed: the manifest schema only   [SERVER-SIDE: the review / holding area]
inline juce::var receiptFor (const juce::File& work, const juce::File& stored, const juce::var& m, const juce::var& account, const juce::String& path, const juce::String& device = {})
{
    const bool genuine = stored.existsAsFile() && m.getProperty ("schema", "").toString() == "ej_contribution/0";
    const auto id = "rcpt_stub_" + juce::String::toHexString (juce::Time::currentTimeMillis());
    auto* r = new juce::DynamicObject(); r->setProperty ("receipt_id", id); r->setProperty ("account_id", account.getProperty ("account_id", {}));
    r->setProperty ("genuine", genuine); r->setProperty ("path", path); r->setProperty ("bytes", stored.getSize());
    r->setProperty ("profiles_received", m.getProperty ("profiles", {}).size()); r->setProperty ("confirmations_received", m.getProperty ("confirmations", {}).size());
    r->setProperty ("crashed_reported", m.getProperty ("crashed", {}).size()); r->setProperty ("needs_category_received", m.getProperty ("needs_category", {}).size());
    const juce::var v (r);
    if (genuine)
    {   // 12 Oct: the votes are recorded; a reference they make agreed (2 Macs) is said on the receipt
        juce::Array<juce::var> na; for (const auto& x : recordVotes (work, m)) na.add (x);
        r->setProperty ("reference_votes_received", m.getProperty ("reference_votes", {}).size()); r->setProperty ("references_now_agreed", na);
        // 13 Oct (Kathy): the upload merges into its RUN (server_stub/runs/<run_id>.json); a final upload (sent_early / finished) completes the
        // account's and the Mac's go; the code is judged on the merged run (rewardFor). An upload with no run is its own finished run (older apps)
        const auto acct = account.getProperty ("account_id", "").toString();
        const bool before = alreadyContributed (work, acct, device);
        const auto runV = m.getProperty ("run", {}); const auto runId = runV.getProperty ("run_id", "").toString().isNotEmpty() ? runV.getProperty ("run_id", "").toString() : "run_" + id;
        const auto kind = runV.isObject() ? runV.getProperty ("kind", "finished").toString() : juce::String ("finished");
        const auto runsDir = work.getChildFile ("server_stub/runs"); runsDir.createDirectory();
        const auto runF = runsDir.getChildFile (juce::File::createLegalFileName (runId) + ".json");
        auto run = juce::JSON::parse (runF.loadFileAsString()); if (! run.isObject()) run = newRun (runId);
        juce::StringArray ids; if (const auto* a = run.getProperty ("plugins", {}).getDynamicObject()) for (const auto& nv : a->getProperties()) if ((bool) nv.value.getProperty ("accepted", false)) ids.add (nv.name.toString());
        const int before_n = ids.size(); acceptedNew (work, stored, m, ids);   // known: the server's db + other runs' accepted plugins; this run's already-accepted never twice
        juce::StringArray fresh; for (int i = before_n; i < ids.size(); ++i) fresh.add (ids[i]);
        mergeRun (run, m, fresh); runF.replaceWithText (juce::JSON::toString (run));
        const auto t = tallyRun (run); const int minimum = (int) serverConfig (work).getProperty ("qualify_minimum", kQualifyMinimumDefault);
        const auto rd = rewardFor (kind, t, minimum, before);
        r->setProperty ("run_id", runId); r->setProperty ("kind", kind); r->setProperty ("run_queued", t.queued); r->setProperty ("run_final", t.final_);
        r->setProperty ("accepted_new", t.accepted); r->setProperty ("accepted_this_upload", fresh.size()); r->setProperty ("reward_minimum", minimum);
        r->setProperty ("reward_eligible", rd.eligible); r->setProperty ("reward_message", rd.message);
        if (kind == "sent_early" || kind == "finished")
        {   // the run is over: its accepted plugins become known to the server; the account and the Mac have taken part
            auto acc = juce::JSON::parse (work.getChildFile ("server_stub/accepted_plugins.json").loadFileAsString()); juce::Array<juce::var> al; if (const auto* a = acc.getArray()) al = *a;
            for (const auto& x : ids) if (! al.contains (x)) al.add (x);
            work.getChildFile ("server_stub/accepted_plugins.json").replaceWithText (juce::JSON::toString (juce::var (al)));
            auto p = participation (work);
            auto add = [&] (const char* k, const juce::String& x) { if (x.isEmpty() || listed (p.getProperty (k, {}), x)) return; auto arr = p.getProperty (k, {}); arr.append (x); p.getDynamicObject()->setProperty (k, arr); };
            add ("accounts", acct); add ("devices", device);
            work.getChildFile ("server_stub/participation.json").replaceWithText (juce::JSON::toString (p));
        }
    }
    const auto dir = work.getChildFile ("server_stub/uploads"); dir.createDirectory();
    dir.getChildFile (id + ".receipt.json").replaceWithText (juce::JSON::toString (v));
    return v;
}
// the upload, both paths: direct (POST /contributions, multipart) up to 4.5 MB; else the signed blob (upload-url -> PUT -> complete)
inline juce::var upload (const juce::File& work, const juce::File& zip, const juce::File& manifest, const juce::var& account, const juce::String& device = {})
{
    // rehearsals: EJ_WIZARD_OFFLINE_FILE names a file whose presence means "no network" - the send fails (void), the app keeps it in its outbox
    if (const char* off = std::getenv ("EJ_WIZARD_OFFLINE_FILE"); off != nullptr && *off && juce::File (off).existsAsFile()) return {};
    const auto m = juce::JSON::parse (manifest.loadFileAsString());
    if (uploadPathFor (zip.getSize(), manifest.getSize()) == UploadPath::direct)
    {   // POST <base>/api/wizard/v1/contributions (multipart: manifest + bundle)
        const auto dir = work.getChildFile ("server_stub/uploads"); dir.createDirectory();
        const auto stored = dir.getChildFile ("direct_" + juce::String::toHexString (juce::Time::currentTimeMillis()) + ".zip"); zip.copyFileTo (stored);
        auto r = receiptFor (work, stored, m, account, "direct", device); r.getDynamicObject()->setProperty ("endpoint", endpoint ("contributions")); return r;
    }
    // 1) POST /contributions/upload-url {bytes, bundle_sha256} -> {upload_url, blob_ref, expires}: the stub's "signed URL" is a local folder
    const auto blobRef = "blob_stub_" + juce::SHA256 (zip).toHexString().substring (0, 16);
    const auto signedDir = work.getChildFile ("server_stub/signed_blob"); signedDir.createDirectory();
    const auto target = signedDir.getChildFile (blobRef + ".zip");
    // 2) PUT the zip to upload_url
    if (! zip.copyFileTo (target)) return {};
    // 3) POST /contributions/complete {blob_ref, manifest} -> the receipt
    auto r = receiptFor (work, target, m, account, "signed_blob", device);
    r.getDynamicObject()->setProperty ("blob_ref", blobRef); r.getDynamicObject()->setProperty ("upload_url", "file://" + target.getFullPathName());
    r.getDynamicObject()->setProperty ("endpoint", endpoint ("contributions/complete"));
    return r;
}
inline juce::var promo (const juce::File& work, const juce::var& account, const juce::var& receipt)
{   // POST /promo {receipt_id} -> {code, issued_now, emailed_to}; one code per account   [SERVER-SIDE: promo rules + issuing + email]
    // 13 Oct (item 7, replacing "a code even when nothing new"): only a receipt the server judged reward_eligible (accepted new >= minimum,
    // the account and the Mac not rewarded before) gets a code
    if (! (bool) receipt.getProperty ("genuine", false) || ! (bool) receipt.getProperty ("reward_eligible", false)) return {};
    const auto ledgerF = work.getChildFile ("server_stub/promo_ledger.json"); auto ledger = juce::JSON::parse (ledgerF.loadFileAsString());
    if (! ledger.isObject()) ledger = juce::var (new juce::DynamicObject());
    const auto acct = account.getProperty ("account_id", "").toString();
    auto* o = new juce::DynamicObject();
    if (ledger.hasProperty (acct)) { o->setProperty ("code", ledger[acct.toRawUTF8()]); o->setProperty ("issued_now", false); }
    else
    {
        const auto code = "EJ-FREE-" + juce::SHA256 ((acct + "|wizard-v1").toUTF8()).toHexString().substring (0, 8).toUpperCase();
        ledger.getDynamicObject()->setProperty (acct, code); ledgerF.getParentDirectory().createDirectory(); ledgerF.replaceWithText (juce::JSON::toString (ledger));
        o->setProperty ("code", code); o->setProperty ("issued_now", true);
        const auto outbox = work.getChildFile ("server_stub/outbox"); outbox.createDirectory();
        outbox.getChildFile (acct + ".txt").replaceWithText ("To: " + account.getProperty ("email", "").toString() + "\nSubject: Your free EchoJay month\n\nThank you for helping EchoJay learn your plugins. Your code: " + code + "\n");
    }
    o->setProperty ("emailed_to", account.getProperty ("email", {}));
    return juce::var (o);
}
} // namespace stub
} // namespace ejmap::wizserver
