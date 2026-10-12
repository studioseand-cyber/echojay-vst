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

namespace ejmap::wizroute
{
// --- the /lookup answer (STUB: the mock database folder) ---------------------------------------------------------------------------
enum class Rule { A, B, C, pendingReview };
struct Answer { Rule rule = Rule::C; juce::String category, why; const wizard::DbEntry* entry = nullptr; };
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
inline Answer ask (const std::vector<wizard::DbEntry>& db, const std::set<juce::String>& pending, const juce::var& catalogue,
                   const juce::String& uid, const juce::String& version, const juce::String& mapFp, const juce::String& name)
{
    Answer a; const auto pid = pluginId (uid, version);
    const auto L = wizard::lookup (db, uid, version, mapFp);
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
struct Decision { Route route = Route::categorise; juce::String category, why; };
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
// the Tracker's kind for a Phase B category (its per-plugin seconds)
inline juce::String kindOf (const juce::String& phaseBCategory) { return phaseBCategory == "wizard" ? juce::String ("compressor") : phaseBCategory == "wizard_eq" ? juce::String ("eq") : phaseBCategory; }
inline Decision route (const Answer& a, const juce::String& nameCategory, const juce::String& outOfScope)
{
    switch (a.rule)
    {
        case Rule::A: return { Route::skipHave, a.category, "already in the database: " + a.why };
        case Rule::pendingReview: return { Route::skipPending, {}, a.why };
        case Rule::B: return { Route::quickCheck, a.category, a.why };
        case Rule::C: break;
    }
    if (a.category.isNotEmpty())
    {   // the server's names (Sean's 12); an internal label from a local catalogue: multiband -> held, the rest out of scope (never sent)
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
