/*
  EjmapLedger.h

  Persistent record of every plugin the tool has touched, plus the crash
  quarantine protocol.

  In-process hosting means a plugin crash takes the whole app down. There is no
  worker to catch it, because the human has to see the editor. So the protocol is
  a file on disk written before instantiation:

      beginLoad(id)   -> writes inflight.json
      endLoad(id, ok) -> deletes inflight.json, appends a ledger record

  If inflight.json exists at launch, the previous run died inside that plugin.
  Mark it crash_on_load, quarantine it, continue.

  Everything writes through immediately. Nothing is held until submit. A session
  that dies mid-plugin loses at most the current row.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapSchema.h"

namespace ejmap
{

struct LedgerRecord
{
    /** The format's own identifier string ("AudioUnit:Effects/aufx,SILM,ksWV",
        or a VST3 bundle path). See ScannedPlugin::pluginId: the XOR-derived
        uniqueId this used to be collides, measured, twice across four Waves
        components. Stable across versions; version is metadata below.
    */
    juce::String pluginId;
    juce::String name, vendor, format, version;
    LoadOutcome  outcome = LoadOutcome::timeout;
    juce::String detail;            // error text, or why it was quarantined
    juce::String at;                // ISO 8601
    int paramCount = 0;

    /** "load" or "scan". Both stages write ledger rows, because both can take
        the process down and both must be attributable. They are NOT the same
        event: a scan probe only asks the format what is in a file, a load
        instantiates and opens an editor.

        M1's outcome summary counts LOAD rows. Counting both would report one
        row per VST3 bundle on the machine as if a human had attempted it.
    */
    juce::String stage = "load";

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("plugin_id", pluginId);
        o->setProperty ("stage", stage);
        o->setProperty ("name", name);
        o->setProperty ("vendor", vendor);
        o->setProperty ("format", format);
        o->setProperty ("version", version);
        o->setProperty ("outcome", toString (outcome));
        o->setProperty ("detail", detail);
        o->setProperty ("at", at);
        o->setProperty ("param_count", paramCount);
        return juce::var (o);
    }
};

//==============================================================================
class Ledger
{
public:
    Ledger()
    {
        root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ejmap");
        root.createDirectory();
        root.getChildFile ("maps").createDirectory();
        root.getChildFile ("screenshots").createDirectory();

        ledgerFile    = root.getChildFile ("ledger.json");
        inflightFile  = root.getChildFile ("inflight.json");
        quarantineFile= root.getChildFile ("quarantine.json");

        loadQuarantine();
    }

    juce::File getRoot() const noexcept { return root; }

    //==========================================================================
    /** Call once at startup, before any scan. Returns the plugin id that killed
        the previous run, or an empty string. Quarantines it as a side effect.
    */
    juce::String recoverFromCrash()
    {
        if (! inflightFile.existsAsFile())
            return {};

        auto v = juce::JSON::parse (inflightFile.loadFileAsString());
        auto id = v.getProperty ("plugin_id", "").toString();

        if (id.isNotEmpty())
        {
            LedgerRecord r;
            r.pluginId = id;
            r.name     = v.getProperty ("name", "").toString();
            r.vendor   = v.getProperty ("vendor", "").toString();
            r.format   = v.getProperty ("format", "").toString();
            r.version  = v.getProperty ("version", "").toString();
            r.stage    = v.getProperty ("stage", "load").toString();
            r.outcome  = LoadOutcome::crashOnLoad;

            // Same outcome, honest about which stage produced it. A VST3 that
            // kills the process while the format is reading its factory did not
            // die "while the editor was opening", and saying so would send the
            // next person looking in the wrong place.
            r.detail   = r.stage == "scan"
                           ? "process died while the format was reading this file during Scan"
                           : "process died while the editor was opening";
            r.at       = nowIso();
            append (r);
            quarantine (id, "crash_on_load");
        }

        inflightFile.deleteFile();
        return id;
    }

    /** Written before any call that hands control to plugin code:
        AudioPluginFormatManager::createPluginInstance on the load path, and
        AudioPluginFormat::findAllTypesForFile on the VST3 scan path.

        stage is "load" or "scan". Scan needs this too: 646 of the 860 VST3
        bundles on the development machine ship no moduleinfo.json, so JUCE
        falls back to opening the module to read its factory. That is plugin
        code running inside a Scan, and without an inflight record a crash there
        leaves nothing naming the file that caused it.
    */
    void beginLoad (const juce::String& pluginId,
                    const juce::String& name,
                    const juce::String& vendor,
                    const juce::String& format,
                    const juce::String& version,
                    const juce::String& stage = "load")
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("plugin_id", pluginId);
        o->setProperty ("name", name);
        o->setProperty ("vendor", vendor);
        o->setProperty ("format", format);
        o->setProperty ("version", version);
        o->setProperty ("stage", stage);
        o->setProperty ("at", nowIso());

        // Must reach the disk before the call it is protecting. A buffered write
        // that is still in memory when the process dies records nothing.
        inflightFile.replaceWithText (juce::JSON::toString (juce::var (o), false));
    }

    void endLoad (LedgerRecord r)
    {
        r.at = nowIso();
        append (r);
        inflightFile.deleteFile();
    }

    //==========================================================================
    bool isQuarantined (const juce::String& pluginId) const
    {
        return quarantined.contains (pluginId);
    }

    void quarantine (const juce::String& pluginId, const juce::String& reason)
    {
        if (quarantined.contains (pluginId))
            return;

        quarantined.add (pluginId);
        quarantineReasons.set (pluginId, reason);
        saveQuarantine();
    }

    /** Deliberately manual. A plugin that crashed once usually crashes again,
        and silently retrying it turns one lost session into a loop.
    */
    void releaseFromQuarantine (const juce::String& pluginId)
    {
        quarantined.removeString (pluginId);
        quarantineReasons.remove (pluginId);
        saveQuarantine();
    }

    juce::StringArray getQuarantined() const { return quarantined; }

    //==========================================================================
    /** Counts by outcome, for the gate in M1. Read from the stored file, never
        from an in-memory tally: the rule adopted in this project is that
        reported numbers are asserted against stored data.
    */
    juce::HashMap<juce::String, int>& getOutcomeCounts (const juce::String& stage = "load")
    {
        counts.clear();
        for (const auto& v : readAll())
        {
            // Rows written before the stage field existed have none; they are
            // load rows, because scan did not write rows then.
            if (v.getProperty ("stage", "load").toString() != stage)
                continue;

            auto k = v.getProperty ("outcome", "").toString();
            counts.set (k, counts[k] + 1);
        }
        return counts;
    }

    juce::Array<juce::var> readAll() const
    {
        juce::Array<juce::var> out;
        if (! ledgerFile.existsAsFile())
            return out;

        // One JSON object per line. Append-only, survives a mid-write crash
        // losing at most the final line.
        juce::StringArray lines;
        lines.addLines (ledgerFile.loadFileAsString());
        for (const auto& line : lines)
        {
            if (line.trim().isEmpty()) continue;
            auto v = juce::JSON::parse (line);
            if (v.isObject()) out.add (v);
        }
        return out;
    }

private:
    void append (const LedgerRecord& r)
    {
        ledgerFile.appendText (juce::JSON::toString (r.toVar(), true) + "\n");
    }

    // quarantine.json is an ARRAY of { plugin_id, reason }, not an object keyed
    // by plugin id. A plugin id is now the format's identifier string, and
    // juce::Identifier permits only [A-Za-z0-9_-:#@$%]: the '/' and ',' in
    // "AudioUnit:Effects/aufx,SILM,ksWV" make it an invalid property name. Using
    // it as a key would assert in debug and write a malformed file in release.
    void loadQuarantine()
    {
        quarantined.clear();
        quarantineReasons.clear();
        if (! quarantineFile.existsAsFile())
            return;

        auto v = juce::JSON::parse (quarantineFile.loadFileAsString());
        if (auto* arr = v.getArray())
        {
            for (const auto& entry : *arr)
            {
                auto id = entry.getProperty ("plugin_id", "").toString();
                if (id.isEmpty())
                    continue;

                quarantined.add (id);
                quarantineReasons.set (id, entry.getProperty ("reason", "").toString());
            }
        }
    }

    void saveQuarantine()
    {
        juce::Array<juce::var> entries;
        for (const auto& id : quarantined)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("plugin_id", id);
            o->setProperty ("reason", quarantineReasons[id]);
            entries.add (juce::var (o));
        }
        quarantineFile.replaceWithText (juce::JSON::toString (juce::var (entries), true));
    }

    static juce::String nowIso()
    {
        return juce::Time::getCurrentTime().toISO8601 (true);
    }

    juce::File root, ledgerFile, inflightFile, quarantineFile;
    juce::StringArray quarantined;
    juce::HashMap<juce::String, juce::String> quarantineReasons;
    juce::HashMap<juce::String, int> counts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Ledger)
};

} // namespace ejmap
