/*
  EjmapWizardRoute.h - NO PLUGIN LEFT BEHIND (Kathy, 12 Oct; feat/ejmap-wizard). Where each plugin goes, decided in this order, the
  cheap answers first so nothing the database can settle is ever loaded:

    (a) THE DATABASE (the /lookup answer, no load): rule A (already mapped) -> skipped entirely; pending_review (another user already
        sent it as needs_category) -> skipped entirely, not re-sent; rule B -> only its quick read-back check.
    (b) rule C WITH A SERVER CATEGORY (Sean-approved): compressor / EQ -> the measuring queue; any other category -> sent as its map
        plus the category (the server learns it exists; the wizard cannot measure it yet).
    (c) the NAME RULES (compressor / EQ words only).
    (d) OUT OF SCOPE by AU type or meter / utility words (EJ Map's categorise rule 1) -> sent as identity + why, never loaded.
    (e) ONLY WHAT IS LEFT gets EJ Map's signature reads (categoriseOne). After them: settled eq -> the EQ queue; settled dynamics ->
        the compressor queue (the compressor certification decides inside dynamics: a gate or a rider comes back "could not be measured",
        and is still reported); another settled category -> sent as map + category; unsettled -> sent as NEEDS_CATEGORY (its map, the
        reads, the leaning) for Sean to label. Nothing is dropped silently.

  Pinned in RoundTripTest.cpp (WZ-ROUTE-*).
*/
#pragma once

#include "EjmapWizard.h"
#include "EjmapWizardServer.h"
#include <juce_core/juce_core.h>
#include <set>
#include <mutex>

namespace ejmap::wizroute
{
// --- the /lookup answer (STUB: the mock database folder) ---------------------------------------------------------------------------
enum class Rule { A, B, C, pendingReview };
// passes (13 Oct, limiters): when not empty, the only Phase B passes still to run (a limiter the database has half of); limiterKnown:
// the database holds its limiter pass, so its compressor pass may run without a limiter row of this Mac's own
struct Answer { Rule rule = Rule::C; juce::String category, why; const wizard::DbEntry* entry = nullptr; juce::StringArray passes; bool limiterKnown = false; };
inline juce::String pluginId (const juce::String& uid, const juce::String& version) { return "AudioUnit|" + uid.toLowerCase() + "|" + version; }
// <db>/pending_review.json: [{"plugin_id": "AudioUnit|uid|version", "product": ...}] - what other users already sent as needs_category
inline std::set<juce::String> loadPending (const juce::File& db)
{
    std::set<juce::String> s; const auto v = juce::JSON::parse (db.getChildFile ("pending_review.json").loadFileAsString());
    if (const auto* a = v.getArray()) for (const auto& e : *a) s.insert (e.getProperty ("plugin_id", "").toString().toLowerCase());
    return s;
}
// the server's category (Sean-approved): <db>/categories.json keyed by plugin_id, else by name
inline juce::String serverCategory (const juce::var& catalogue, const juce::String& pid, const juce::String& name)
{
    const auto byId = catalogue.getProperty (juce::Identifier (pid), {}).toString(); if (byId.isNotEmpty()) return byId;
    return catalogue.getProperty (juce::Identifier (name), {}).toString();
}
inline juce::StringArray phaseBFor (const juce::String& c);
inline Answer ask (const std::vector<wizard::DbEntry>& db, const std::set<juce::String>& pending, const juce::var& catalogue,
                   const juce::String& uid, const juce::String& version, const juce::String& mapFp, const juce::String& name)
{
    Answer a; const auto pid = pluginId (uid, version);
    const auto L = wizard::lookup (db, uid, version, mapFp);
    if (L.match == wizard::Match::exact && L.entry->category() == "limiter")
    {   // THE LIMITER'S TWO PASSES COUNT SEPARATELY (Kathy, 13 Oct): the limiter pass and its compressor pass, each looked up on its own
        juce::StringArray have;
        for (const auto& e : db)
            if (e.uid() == uid.toLowerCase() && e.version() == version && (mapFp.isEmpty() || e.mapFp().isEmpty() || e.mapFp() == mapFp) && e.pass.isNotEmpty()) have.addIfNotAlreadyThere (e.pass);
        juce::StringArray missing; for (const auto& pb : phaseBFor ("limiter")) if (! have.contains (pb)) missing.add (pb);
        if (! missing.isEmpty())
        {
            a.rule = Rule::C; a.category = "limiter"; a.passes = missing; a.limiterKnown = have.contains ("limiter");
            a.why = "the database has its " + have.joinIntoString (" and ") + " pass; still to measure: " + missing.joinIntoString (" and "); return a;
        }
    }
    if (L.match == wizard::Match::exact) { a.rule = Rule::A; a.entry = L.entry; a.category = L.entry->category(); a.why = L.why; return a; }
    if (pending.count (pid.toLowerCase())) { a.rule = Rule::pendingReview; a.why = "already sent for review by another user"; return a; }
    if (L.match == wizard::Match::sameControls) { a.rule = Rule::B; a.entry = L.entry; a.category = L.entry->category(); a.why = L.why; return a; }
    a.rule = Rule::C; a.category = serverCategory (catalogue, pid, name); a.why = L.why; return a;
}

// --- the route -----------------------------------------------------------------------------------------------------------------------
enum class Route { skipHave, skipPending, quickCheck, measure, sendOther, outOfScope, categorise, needsCategory, held };   // held (12 Oct): multiband - no server category yet
inline juce::String routeName (Route r)
{
    switch (r) { case Route::skipHave: return "skip_have"; case Route::skipPending: return "skip_pending"; case Route::quickCheck: return "quick_check";
                 case Route::measure: return "measure"; case Route::sendOther: return "send_other"; case Route::outOfScope: return "out_of_scope";
                 case Route::categorise: return "categorise"; case Route::needsCategory: return "needs_category"; case Route::held: return "held"; }
    return {};
}
struct Decision { Route route = Route::categorise; juce::String category, why; juce::StringArray passes; bool limiterKnown = false; };   // passes: see Answer
// THE KNOWN MULTIBANDS (Kathy, 12 Oct): the server has no multiband category, and a multiband with none reads as "dynamics" and
// spends ~10 min in the compressor certification before it refuses (OTT, 591 s). So the wizard ships EJ Map's own list - the 17
// products Sean's cert found multiband (cert outcomes state "multiband", 8-9 Oct) - by plugin uid (any version): held straight away.
inline const std::set<juce::String>& knownMultibandUids()
{
    static const std::set<juce::String> k { "4e436063", "4e43607d",                                   // C4 (m) / (s)
                                            "49306063", "4930607d", "49306263", "4930627d",           // C6, C6-SideChain (m) / (s)
                                            "26a5d6d",                                                // Drawmer 1973
                                            "697c7f2c",                                               // DynOne3
                                            "46450463", "4645047d",                                   // LinMB (m) / (s)
                                            "6f457234", "6f6a7363",                                   // Lindell 354E, Lindell MBC
                                            "7f56566b", "6155777e",                                   // MO-TT, OTT
                                            "526d5652", "61444242", "75154755" };                     // Ozone 12 Dynamics, Pro-MB, SSL G3 MultiBusComp
    return k;
}
inline bool isKnownMultiband (const juce::String& uid) { return knownMultibandUids().count (uid.toLowerCase()) > 0; }
// the database's multiband list (13 Oct, the beta snapshot: <db>/multiband.json {"uids": [...]}) - added to the built-in list, never replacing it
inline std::set<juce::String> loadMultiband (const juce::File& db)
{
    std::set<juce::String> s; const auto v = juce::JSON::parse (db.getChildFile ("multiband.json").loadFileAsString());
    if (const auto* a = v.getProperty ("uids", {}).getArray()) for (const auto& u : *a) s.insert (u.toString().toLowerCase());
    return s;
}
inline bool isMultiband (const std::set<juce::String>& fromDb, const juce::String& uid) { return isKnownMultiband (uid) || fromDb.count (uid.toLowerCase()) > 0; }
// THE PLAN (13 Oct): what the database step decides, in three words - skip (nothing loads), measure (queued, or rule B's quick read-back
// first), sound-test (EJ Map's signature reads: test tones through the plugin to find its category); held / sent-other load for the map only
inline juce::String planWord (Route r)
{
    switch (r) { case Route::skipHave: case Route::skipPending: case Route::outOfScope: return "skip";
                 case Route::measure: case Route::quickCheck: return "measure";
                 case Route::categorise: return "sound_test";
                 case Route::held: case Route::sendOther: return "map_only";
                 case Route::needsCategory: return "needs_category"; }
    return {};
}
// ALL CATEGORIES (12 Oct): the EJ Map Phase B categories that measure a category - the same modes, outcomes, acceptance and serial lane
// as the certification. A limiter is measured twice: the limiter mode and the compressor certification (limiter_comp, Kathy 10 Oct).
inline juce::StringArray phaseBFor (const juce::String& c)
{
    if (c == "compressor") return { "wizard" };
    if (c == "eq") return { "wizard_eq" };
    if (c == "limiter") return { "limiter", "limitercomp" };
    if (c == "de-esser" || c == "deesser") return { "deesser" };
    if (c == "saturation" || c == "amp_sim") return { "saturation" };
    if (c == "gate") return { "gate" };
    if (c == "transient_shaper" || c == "transient") return { "transient" };
    // multiband: NOT measured by the wizard for now (Kathy, 12 Oct: no server category) - held, listed with its map
    if (c == "pitch" || c == "tuner") return { "tuners" };
    if (c == "reverb") return { "reverb" };
    if (c == "delay") return { "delay" };
    if (c == "channel_strip" || c == "strip") return { "strips" };
    return {};   // modulation, meters, utilities, ...: not measured (sent with the category)
}
inline bool measurable (const juce::String& c) { return ! phaseBFor (c).isEmpty(); }
// THE MEASURING ORDER (Kathy, 12 Oct): fastest and most useful first, so a user who stops early has given the most valuable results.
// Each inner list runs as one Phase B pass (--jobs within it); a stop ends the order there. limitercomp is its own pass AFTER limiter
// (12 Oct): its work list keeps only limiters whose limiter row is ok (phaseb::compressorPassAllowed), so the limiter rows must exist first.
inline const std::vector<juce::StringArray>& measureOrder()
{
    static const std::vector<juce::StringArray> k { { "wizard" }, { "wizard_eq" }, { "deesser" }, { "limiter" }, { "limitercomp" }, { "saturation" },
                                                    { "gate", "transient" }, { "tuners" }, { "reverb", "delay" }, { "strips" } };
    return k;
}
// a limiter's compressor pass (limitercomp) is sent like any compressor: its profile exported AND its tone check passed at g 2
inline bool sendCompressorPass (const juce::String& certState, bool toneCheckPassed) { return certState == "exported" && toneCheckPassed; }
// a draft the server can USE (12 Oct: bx_rockrack's saturation draft read "no control with the drive verdict: nothing for the server to
// use" and was counted measured): a saturation draft needs its drive; the other kinds carry their measured blocks
inline bool draftUsable (const juce::String& phaseBCategory, const juce::var& draft)
{
    if (! draft.isObject()) return false;
    if (phaseBCategory == "saturation") return ! draft.getProperty ("drive", {}).isVoid();
    return true;
}
// "N measured so far" (Kathy, 13 Oct; the rehearsal counted OneKnob Pumper, an ok row the certification then refused as a ducker): a landed
// row counts as measured only when it gave a result - a compressor (or a limiter's compressor pass) its profile exported, an EQ its
// acceptance passed, any other category an ok row (its draft is derived after the measuring; the final summary decides)
inline bool landedMeasured (const juce::String& pb, const juce::String& outcome, const juce::String& certState, bool eqAccepted)
{
    if (outcome != "ok") return false;
    if (pb == "wizard" || pb == "limitercomp") return certState == "exported";
    if (pb == "wizard_eq") return eqAccepted;
    return true;
}
// THE DEMO CHECK IN MEASURING (Kathy, 14 Oct): the plugins a pass checks first (every product queued in the group's categories, once each)
inline juce::StringArray demoNamesForGroup (const juce::StringArray& group, const std::map<juce::String, juce::StringArray>& queues)
{
    juce::StringArray n; for (const auto& pb : group) if (const auto it = queues.find (pb); it != queues.end()) for (const auto& x : it->second) n.addIfNotAlreadyThere (x);
    return n;
}
// a demo leaves EVERY queue (a limiter's compressor pass in a later group too); returns the (queue, product) pairs it left
inline std::vector<std::pair<juce::String, juce::String>> dropDemos (std::map<juce::String, juce::StringArray>& queues, const juce::StringArray& demos)
{
    std::vector<std::pair<juce::String, juce::String>> gone;
    for (auto& [pb, l] : queues) for (const auto& d : demos) if (l.contains (d)) { l.removeString (d); gone.push_back ({ pb, d }); }
    return gone;
}
// LICENCE WINDOWS DURING THE SCAN (Kathy, 14 Oct). What can be known WITHOUT a load:
//   * PACE / iLok wrapping, from the bundle (paceHeld's isPaceWrapped) - but not whether that plugin is licensed: iLok Cloud and machine
//     licences live encrypted in PACE's LicenseSupport. So a PACE-wrapped plugin is skipped unloaded only when the Mac has NO iLok at
//     all: no USB iLok and PACE's licence software not installed (then no PACE licence can exist on it)
//   * a plugin that already raised a licence window on this Mac (remembered in <out>/licence_windows.json): never loaded again
// Anything else may still show one window, once.
// PACE wrapping that is OPTIONAL (14 Oct, measured here): MeldaProduction bundles carry __Pace_Eden.bundle as an optional iLok route, but
// unlicensed Melda plugins run as demos and never raise a window (13 Oct: MTurboComp, MLimiterX, MFreeformEqualizer) - never skipped for it
inline bool paceOptionalVendor (const juce::String& vendor) { return vendor.trim().equalsIgnoreCase ("MeldaProduction"); }
enum class LicenceSkip { none, remembered, paceNoIlok };
inline LicenceSkip licenceSkip (bool rememberedWindow, bool paceWrapped, bool ilokUsb, bool paceSupportInstalled)
{
    if (rememberedWindow) return LicenceSkip::remembered;
    if (paceWrapped && ! ilokUsb && ! paceSupportInstalled) return LicenceSkip::paceNoIlok;
    return LicenceSkip::none;
}
inline juce::String licenceSkipWhy (LicenceSkip s)
{
    if (s == LicenceSkip::remembered) return "needs a licence - skipped (it showed a licence window here before; not loaded again)";
    if (s == LicenceSkip::paceNoIlok) return "needs a licence - skipped (iLok-protected, and this Mac has no iLok: not loaded)";
    return {};
}
// a load that showed a window: the probe's uiShown at the map / quick check, or the signature reads' "a window at the load"
inline bool rowShowedWindow (const juce::String& state, const juce::String& why) { return state == "needs_licence" || why.startsWith ("a window"); }
inline std::set<juce::String> loadLicenceMemory (const juce::File& f)
{
    std::set<juce::String> s; const auto v = juce::JSON::parse (f.loadFileAsString());
    if (const auto* a = v.getArray()) for (const auto& x : *a) s.insert (x.toString().toLowerCase());
    return s;
}
inline void rememberLicenceWindow (const juce::File& f, const juce::String& pluginId)
{
    auto s = loadLicenceMemory (f); if (! s.insert (pluginId.toLowerCase()).second) return;
    juce::Array<juce::var> a; for (const auto& x : s) a.add (x); f.replaceWithText (juce::JSON::toString (juce::var (a)));
}
// THE WAVESHELL GUARD (Kathy, 14 Oct). Waves plugins live inside shared WaveShell bundles (this Mac: WaveShell2-AU 12.0 holds 589 of 616).
// Measured here: a load takes 3.7 s, the first as quick as the rest - so no longer first-load limit is needed. But if a SHELL hangs or
// fails (a broken install, a licence check that never answers), every one of its plugins would wait the full limit. So: a WaveShell load
// gives up after kWaveShellLoadTimeoutMs; one hang, or two crashes, marks the SHELL bad for this scan and its other plugins are skipped
// unloaded - "Waves plugins skipped - check Waves Central". A licence WINDOW is not a shell failure (the licence memory handles it).
inline constexpr int kWaveShellLoadTimeoutMs = 30000;
inline juce::String waveShellOf (const juce::String& bundleFileName)
{ return bundleFileName.startsWithIgnoreCase ("WaveShell") ? bundleFileName.upToLastOccurrenceOf (".component", false, false) : juce::String(); }
inline juce::String wavesSkippedWhy (const juce::String& shell, const juce::String& what) { return "Waves plugins skipped - check Waves Central (" + shell + " " + what + ")"; }
struct ShellGuard
{
    bool isBad (const juce::String& shell) { std::lock_guard<std::mutex> g (m); return shell.isNotEmpty() && bad.count (shell) > 0; }
    juce::String why (const juce::String& shell) { std::lock_guard<std::mutex> g (m); const auto it = bad.find (shell); return it == bad.end() ? juce::String() : it->second; }
    // a load in the shell ended: returns true when this marks the shell bad (one hang, or the second crash)
    bool report (const juce::String& shell, bool timedOut, bool crashed)
    {
        if (shell.isEmpty() || (! timedOut && ! crashed)) return false;
        std::lock_guard<std::mutex> g (m); if (bad.count (shell)) return false;
        if (timedOut) { bad[shell] = "did not finish loading"; return true; }
        if (++crashes[shell] >= 2) { bad[shell] = "crashed twice while loading"; return true; }
        return false;
    }
    std::mutex m; std::map<juce::String, int> crashes; std::map<juce::String, juce::String> bad;
};
// THE READ-BACK CHECK's short cut (14 Oct, CLA-76 (m): 12 loads, 49 s): a written control the plugin does not have fails the check from the
// control list alone - no read loads
inline juce::String missingWrittenControl (const std::vector<wizard::Write>& writes, const std::map<juce::String, int>& names)
{
    for (const auto& w : writes) if (! names.count (w.control)) return w.control;
    return {};
}
// the Tracker's kind for a Phase B category (its per-plugin seconds)
inline juce::String kindOf (const juce::String& phaseBCategory) { return phaseBCategory == "wizard" ? juce::String ("compressor") : phaseBCategory == "wizard_eq" ? juce::String ("eq") : phaseBCategory; }
inline Decision route (const Answer& a, const juce::String& nameCategory, const juce::String& outOfScope, bool knownMultiband = false)
{
    switch (a.rule)
    {
        case Rule::A: return { Route::skipHave, a.category, "already in the database: " + a.why };
        case Rule::pendingReview: return { Route::skipPending, {}, a.why };
        case Rule::B: case Rule::C: break;
    }
    // a known multiband: held at once - never quick-checked, signature-read or queued for the compressor certification
    if (knownMultiband) return { Route::held, "multiband", "held: no server category (a known multiband: EJ Map's list)" };
    if (a.rule == Rule::B) return { Route::quickCheck, a.category, a.why };
    if (a.category.isNotEmpty())
    {   // the server's names (Sean's 12); an internal label from a local catalogue: multiband -> held, the rest out of scope (never sent)
        if (! a.passes.isEmpty()) return { Route::measure, a.category, a.why, a.passes, a.limiterKnown };   // a limiter the database has half of
        if (measurable (a.category)) return { Route::measure, a.category, "the server's category: " + a.category };
        if (a.category == "multiband") return { Route::held, "multiband", "held: no server category (multiband is not measured by the wizard yet)" };
        if (wizserver::isServerCategory (a.category)) return { Route::sendOther, a.category, "the server's category (" + a.category + ") is not measured by the wizard yet: sent so EchoJay knows it" };
        return { Route::outOfScope, a.category, "not an EchoJay category (" + a.category + "): not loaded" };
    }
    if (nameCategory.isNotEmpty()) return { Route::measure, nameCategory, "its name says " + nameCategory };
    if (outOfScope.isNotEmpty()) return { Route::outOfScope, outOfScope, "out of scope (" + outOfScope + "): not loaded" };
    return { Route::categorise, {}, "no category from the server or the name: the signature reads decide" };
}
// the one route that loads a plugin to categorise it (the pin: an already-mapped or pending_review plugin never gets it)
inline bool loadsToCategorise (const Decision& d) { return d.route == Route::categorise; }
// the one route that loads nothing at all
inline bool loadsNothing (const Decision& d) { return d.route == Route::skipHave || d.route == Route::skipPending || d.route == Route::outOfScope; }

// after the signature reads (EJ Map's categorise::decide, or its out-of-scope / licence refusals)
inline Decision afterSignature (bool readsRan, bool settled, const juce::String& category, const juce::String& outOfScope, const juce::String& why)
{
    if (outOfScope.isNotEmpty()) return { Route::outOfScope, outOfScope, "out of scope (" + outOfScope + ")" };
    if (! readsRan || ! settled) return { Route::needsCategory, category, why };   // a licence window or no signature: Sean labels it from the map + reads
    if (category == "eq") return { Route::measure, "eq", "the signature: " + why };
    if (category == "dynamics") return { Route::measure, "compressor", "the signature (dynamics; the compressor certification decides): " + why };
    if (measurable (category)) return { Route::measure, category, "the signature: " + why };   // saturation, reverb, delay, pitch (12 Oct: all categories)
    if (wizserver::isServerCategory (category)) return { Route::sendOther, category, "the signature (" + category + ", not measured by the wizard yet): " + why };
    return { Route::outOfScope, category, "the signature reads " + category + " - not an EchoJay category: " + why };   // modulation: an internal label, never sent
}

// the summary's words for these live in wizard::summaryLine (needs_category / other_category / out_of_scope / pending_review)
} // namespace ejmap::wizroute
