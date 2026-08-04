/*
  EjmapMarks.h

  Two operator marks on a plugin row, stored beside the ledger and NOT in
  map-state.json. That file is a fetch CACHE -- saveMapStateCache rebuilds its
  whole `identities` object from the last server answer -- so a mark written
  there would survive until the next refresh and then vanish, which is the
  worst possible behaviour for a record whose whole job is to persist.

  ============================ TWO KEY SHAPES ============================

  READ THIS BEFORE CHANGING EITHER. The two marks are keyed DIFFERENTLY, on
  purpose, and one record holding two key shapes is exactly the thing someone
  collapses later into "just use the identity".

    issue       ->  format|uid|version      (the FULL identity)
    unmappable  ->  format|uid              (the PRODUCT, version dropped)
    mapped      ->  format|uid|version      (the full identity, elsewhere)

  An ISSUE is about a build: "bands would not infer" is a statement about the
  parameters this version exposes, and a new version may fix it. Carrying it
  forward would hide a fix.

  UNMAPPABLE is about a PRODUCT: Auto-Key is a key detector in 1.0 and in 2.0,
  and a mapper who has decided it is not a mixing processor should not be asked
  again when the vendor ships an update. Carrying it forward is the point.

  ========================================================================

  ADVISORY, NEVER A LOCK. Neither mark gates loading. A greyed row must stay
  loadable, for the same reason `submitted` does not stop a re-map: a mark is
  one person's recommendation, it can be wrong, and the person best placed to
  find that out is the next one to open the plugin. Clearing is the same
  gesture that set it.

  SHAPED TO UPLOAD UNCHANGED. Each entry is {by, at} under a key the server can
  store as-is. When the transport exists (a POST beside the ingest one, and
  identities= returning the flag) this file becomes the local half of a sync
  rather than something to migrate.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <map>

namespace ejmap
{

struct Marks
{
    struct Entry { juce::String by, at; };

    std::map<juce::String, Entry> issues;        // keyed format|uid|version
    std::map<juce::String, Entry> unmappable;    // keyed format|uid

    //==========================================================================
    /** The full identity: what `mapped` and `issue` key on. Mirrors
        echojay::identityKeyForDescription, which is what the columns use.
    */
    static juce::String identityKey (const juce::PluginDescription& d)
    {
        return d.pluginFormatName + "|" + juce::String::toHexString (d.uniqueId) + "|" + d.version;
    }

    /** The product, version dropped: what `unmappable` keys on. */
    static juce::String productKey (const juce::PluginDescription& d)
    {
        return d.pluginFormatName + "|" + juce::String::toHexString (d.uniqueId);
    }

    //==========================================================================
    static juce::File fileIn (const juce::File& root) { return root.getChildFile ("marks.json"); }

    static Marks load (const juce::File& root)
    {
        Marks m;
        auto v = juce::JSON::parse (fileIn (root).loadFileAsString());
        auto read = [] (const juce::var& src, std::map<juce::String, Entry>& dst)
        {
            if (auto* o = src.getDynamicObject())
                for (auto& kv : o->getProperties())
                    dst[kv.name.toString()] = { kv.value.getProperty ("by", "").toString(),
                                                kv.value.getProperty ("at", "").toString() };
        };
        read (v.getProperty ("issues", juce::var()), m.issues);
        read (v.getProperty ("unmappable", juce::var()), m.unmappable);
        return m;
    }

    void save (const juce::File& root) const
    {
        auto* o = new juce::DynamicObject();
        auto write = [] (const std::map<juce::String, Entry>& src)
        {
            auto* d = new juce::DynamicObject();
            for (const auto& kv : src)
            {
                auto* e = new juce::DynamicObject();
                e->setProperty ("by", kv.second.by);
                e->setProperty ("at", kv.second.at);
                d->setProperty (kv.first, juce::var (e));
            }
            return juce::var (d);
        };
        o->setProperty ("issues", write (issues));
        o->setProperty ("unmappable", write (unmappable));
        fileIn (root).replaceWithText (juce::JSON::toString (juce::var (o), false));
    }

    //==========================================================================
    bool hasIssue (const juce::PluginDescription& d) const
    { return issues.find (identityKey (d)) != issues.end(); }

    bool isUnmappable (const juce::PluginDescription& d) const
    { return unmappable.find (productKey (d)) != unmappable.end(); }

    /** Toggle, because clearing is the same gesture that set it. Returns the
        state AFTER the toggle, which is what the caller reports.
    */
    bool toggleIssue (const juce::PluginDescription& d, const juce::String& by)
    {
        const auto k = identityKey (d);
        if (issues.erase (k) > 0) return false;
        issues[k] = { by, juce::Time::getCurrentTime().toISO8601 (true) };
        return true;
    }

    bool toggleUnmappable (const juce::PluginDescription& d, const juce::String& by)
    {
        const auto k = productKey (d);
        if (unmappable.erase (k) > 0) return false;
        unmappable[k] = { by, juce::Time::getCurrentTime().toISO8601 (true) };
        return true;
    }
};

} // namespace ejmap
