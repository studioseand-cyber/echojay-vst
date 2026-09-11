#pragma once
#include <JuceHeader.h>
#include "EJStateRoot.h"   // 6 Sep 2026: every user-state path resolves through the isolatable root

// WHY a uid is in plugin_disabled.json (29 Aug 2026).
//
// THE GAP THIS CLOSES. plugin_disabled.json is a bare array of uids and records
// nothing about how each one got there. Two rows on this machine -- SSL Native
// X-EQ 2 (1 of 29 SSL rows) and Weiss Deess (1 of 8 Softube) -- carry the
// signature of a load-failure exclusion: lone disabled rows among enabled
// siblings, both iLok vendors. SIGNATURE IS NOT PROVENANCE. Nothing in the file
// can distinguish "the user unticked it in Settings" from "it failed to load
// once and the user pressed Don't suggest again", so the question could not be
// answered, only guessed at. This makes it answerable from now on.
//
// A SIDECAR, NOT A SCHEMA CHANGE, and that is deliberate. Turning
// plugin_disabled.json into an array of objects would break every reader it
// has: PluginScanner::loadEnabledState and maybeReloadEnabledState both do
// `json.getArray()` and take each item as a uid string, and mapfps_test reads
// it the same way. Those readers stay byte-compatible; the reasons live beside
// them in plugin_disabled_reasons.json, keyed by the same uid:
//
//   { "<uid>": { "why": "settings" | "load-failure", "at": "2026-08-29" } }
//
// ROWS THAT PREDATE THE FIELD HAVE NO ENTRY, and that is the correct answer
// rather than a defect: their reason genuinely is not known, and inventing one
// would be worse than the silence this replaces. Every reader must treat a
// missing entry as UNKNOWN, never as a default reason -- disableReasonFor
// returns an empty string for it, and the two live rows above will keep
// returning empty forever. See [[an-absence-needs-its-presence-condition]]:
// the presence condition here is "disabled on or after 29 Aug 2026".
//
// Header-inline, the EJDialMissRows.h discipline: the gate links the PREVIOUS
// build's SharedCode, so anything a pin exercises has to be compiled by the
// test TU rather than linked from a stale archive. It also keeps PluginScanner's
// layout untouched, which is what lets these pins run without a rebuild.
namespace echojay
{

/** Why a uid is disabled. Two values, because there are two doors. */
inline const char* kDisableWhySettings    = "settings";      // unticked in Settings
inline const char* kDisableWhyLoadFailure = "load-failure";  // "Don't suggest again"

inline juce::File disableReasonsFile()
{
    return echojay::userAppData()
             .getChildFile ("EchoJay").getChildFile ("plugin_disabled_reasons.json");
}

/** The whole map, or an empty object. Never throws on a malformed file. */
inline juce::var readDisableReasons()
{
    auto f = disableReasonsFile();
    if (! f.existsAsFile()) return juce::var (new juce::DynamicObject());
    auto v = juce::JSON::parse (f.loadFileAsString());
    return v.getDynamicObject() != nullptr ? v : juce::var (new juce::DynamicObject());
}

/** The recorded reason for one uid, or EMPTY when unknown. Empty is a real
    answer (a row written before this file existed), never a default. */
inline juce::String disableReasonFor (const juce::String& uid)
{
    auto all = readDisableReasons();
    if (auto* o = all.getDynamicObject())
        if (o->hasProperty (juce::Identifier (uid)))
            return o->getProperty (juce::Identifier (uid))
                    .getProperty ("why", juce::var()).toString();
    return {};
}

/** Stamp `why` and today's date against each uid, merging into what is there.
    Called at BOTH disable sites; a uid disabled twice keeps the latest reason. */
inline void recordDisableReasons (const juce::StringArray& uids, const juce::String& why)
{
    if (uids.isEmpty()) return;
    auto all = readDisableReasons();
    auto* o = all.getDynamicObject();
    if (o == nullptr) return;
    const auto at = juce::Time::getCurrentTime().formatted ("%Y-%m-%d");
    for (const auto& u : uids)
    {
        if (u.isEmpty()) continue;
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("why", why);
        e->setProperty ("at",  at);
        o->setProperty (juce::Identifier (u), juce::var (e.get()));
    }
    disableReasonsFile().getParentDirectory().createDirectory();
    disableReasonsFile().replaceWithText (juce::JSON::toString (all, true));
}

/** Drop the reasons for uids being RE-ENABLED: the row is no longer disabled,
    so a stale reason would outlive its subject and answer a question nobody
    asked. Silent no-op when none of them are recorded. */
inline void clearDisableReasons (const juce::StringArray& uids)
{
    if (uids.isEmpty()) return;
    auto all = readDisableReasons();
    auto* o = all.getDynamicObject();
    if (o == nullptr) return;
    bool changed = false;
    for (const auto& u : uids)
        if (o->hasProperty (juce::Identifier (u)))
        { o->removeProperty (juce::Identifier (u)); changed = true; }
    if (! changed) return;
    disableReasonsFile().replaceWithText (juce::JSON::toString (all, true));
}

} // namespace echojay
