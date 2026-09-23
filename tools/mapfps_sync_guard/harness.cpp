// mapfps_sync_guard — 21q item 1 (23 Sep 2026): the inventory sync, client half.
//
// THE DEFECT. A fingerprint is only learned by LOADING a plugin, so on Sean's machine mapFps named 63 binaries out
// of a 1429-plugin inventory and every fp-keyed path on the server answered from the sibling-merged view for the
// other 1366. THE FIX. POST the inventory to /api/params/sync, store {fp, version, tier} per identity, and let
// mapFps fall back to that store - a probe-derived fingerprint, measured here from the binary the user holds,
// always winning over a synced one. Wire contract: ~/echojay-saas/CONTRACT_SYNC_2026-09-23.md.
//
// FIXTURE = the shape of the complaint: 1429 entries, 63 of them fingerprinted locally, a stub sync answering 1062
// identities (the 63 among them, so the override rule is exercised by the same run).
// RED as it stood: buildMapFpsJson cannot see a synced store at all - applySyncedIdentities does not exist, and the
// count stays at 63. The network is never touched here: the stub IS the server's answer.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EchoJayParamMaps.h"
#include "EJStateRoot.h"
#include <cstdio>

namespace {
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}

constexpr int kFeed   = 1429;   // the inventory
constexpr int kProbed = 63;     // fingerprinted locally (the measured before-figure)
constexpr int kSynced = 1062;   // what the stub server answers, the 63 included

juce::String nameFor (int i)    { return "Fixture Plug " + juce::String (i); }
juce::String versionFor (int i) { return "1." + juce::String (i % 9); }
int          uidFor (int i)     { return 0x10000 + i; }          // never 0: a zero uid is no identity

juce::PluginDescription descFor (int i)
{
    juce::PluginDescription d;
    d.name = nameFor (i);
    d.pluginFormatName = "AudioUnit";
    d.category = "Effect";
    d.manufacturerName = "Fixture Audio";
    d.version = versionFor (i);
    d.uniqueId = uidFor (i);
    d.deprecatedUid = uidFor (i);
    d.fileOrIdentifier = "fixture:" + juce::String (i);
    d.isInstrument = false;
    d.numInputChannels = 2;
    d.numOutputChannels = 2;
    return d;
}

juce::String ikFor (int i) { return echojay::identityKeyForDescription (descFor (i)); }
juce::String probeFpFor  (int i) { return ikFor (i) + "|100"; }   // what a load would have measured
juce::String serverFpFor (int i) { return ikFor (i) + "|200"; }   // what the server answers

// The entries cache is the only door into ChainHost::entries_, so the fixture writes one and loads it.
void writeEntriesCache()
{
    juce::XmlElement root ("CHAIN_ENTRIES");
    root.setAttribute ("scannedAt", juce::String (juce::Time::getCurrentTime().toMilliseconds()));
    for (int i = 0; i < kFeed; ++i)
        root.addChildElement (descFor (i).createXml().release());
    auto f = ChainHost::getEntriesCacheFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (root.toString());
}

std::vector<ScannedPlugin> scannerRows()
{
    std::vector<ScannedPlugin> rows;
    rows.reserve ((size_t) kFeed);
    for (int i = 0; i < kFeed; ++i)
    {
        ScannedPlugin p;
        p.name = nameFor (i);
        p.manufacturer = "Fixture Audio";
        p.format = "AudioUnit";
        p.category = "Effect";
        p.path = "fixture:" + juce::String (i);
        p.uid = juce::String::toHexString (uidFor (i));
        p.enabled = true;
        rows.push_back (p);
    }
    return rows;
}

int entryCount (const juce::String& mapFpsJson)
{
    const auto v = juce::JSON::parse (mapFpsJson);
    auto* o = v.getDynamicObject();
    return o == nullptr ? 0 : o->getProperties().size();
}
juce::String fpOf (const juce::String& mapFpsJson, const juce::String& name)
{
    const auto v = juce::JSON::parse (mapFpsJson);
    auto* o = v.getDynamicObject();
    return o == nullptr ? juce::String() : o->getProperty (juce::Identifier (name)).toString();
}
} // namespace

int main()
{
    echojay::requireIsolationOrDie ("mapfps_sync_guard");
    std::printf ("mapfps_sync_guard: the inventory sync fills mapFps for plugins this machine has never loaded\n");
    std::printf ("== 21q item 1: %d-plugin feed, %d fingerprinted locally, a stub sync answering %d ==\n",
                 kFeed, kProbed, kSynced);

    // The scribble leg re-runs this binary in the SAME isolated HOME, and leg D deliberately proves the store is
    // PERSISTED - so without this the second leg starts with 1062 already stored and leg A measures the wrong
    // machine. Isolated root only (requireIsolationOrDie above), so this can never touch a real cache.
    ChainHost::getParamMapsCacheFile().deleteFile();
    writeEntriesCache();
    auto host = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
    host->loadFromDisk();
    host->buildRecommendable (scannerRows(), {});
    check (host->getRecommendableCount() == kFeed, "fixture: the feed carries every entry",
           juce::String (host->getRecommendableCount()) + " of " + juce::String (kFeed));

    // The 63 a load would have fingerprinted on this machine.
    for (int i = 0; i < kProbed; ++i)
        host->indexIdentityFp (ikFor (i), probeFpFor (i));

    // ---- BEFORE: the measured complaint -----------------------------------------------------------------
    const auto before = host->buildMapFpsJson();
    check (entryCount (before) == kProbed,
           "A. before the sync, mapFps names ONLY what this machine has loaded  (the complaint)",
           juce::String (entryCount (before)) + " entr(ies)");
    check (fpOf (before, nameFor (0)) == probeFpFor (0),
           "A. ...and those entries carry the fingerprint the load measured", fpOf (before, nameFor (0)));
    check (fpOf (before, nameFor (kFeed - 1)).isEmpty(),
           "A. ...while a plugin never loaded is simply absent", nameFor (kFeed - 1));

    // ---- THE STUB ANSWER --------------------------------------------------------------------------------
    // Exactly what /api/params/sync returns, already parsed: identity -> {fp, version, tier}. The first 63 overlap
    // the probed set on purpose, so this run also proves which source wins.
    std::map<juce::String, echojay::SyncedIdentity> answer;
    for (int i = 0; i < kSynced; ++i)
        answer[ikFor (i)] = { serverFpFor (i), versionFor (i), "exact" };
    answer["AudioUnit|deadbeef|9.9"] = { {}, "9.9", "none" };   // an identity with no fp is NOT an answer
    host->applySyncedIdentities (answer);
    check (host->syncedIdentityCount() == kSynced,
           "B. the store keeps every answered identity and refuses the one with no fp",
           juce::String (host->syncedIdentityCount()) + " stored of " + juce::String ((int) answer.size()) + " rows");

    // ---- AFTER: the whole point -------------------------------------------------------------------------
    const auto after = host->buildMapFpsJson();
    check (entryCount (after) >= kSynced,
           "C. mapFps now names every synced plugin  (RED as it stood: the synced store did not exist)",
           juce::String (entryCount (after)) + " entr(ies), was " + juce::String (entryCount (before)));
    check (fpOf (after, nameFor (kProbed + 10)) == serverFpFor (kProbed + 10),
           "C. ...a plugin this machine never loaded carries the SERVER's fingerprint",
           fpOf (after, nameFor (kProbed + 10)));
    check (fpOf (after, nameFor (0)) == probeFpFor (0),
           "C. ...and where both know it, the PROBE-DERIVED fingerprint wins",
           fpOf (after, nameFor (0)) + " (server said " + serverFpFor (0) + ")");
    check (fpOf (after, nameFor (kFeed - 1)).isEmpty(),
           "C. ...a plugin neither source knows stays absent, never guessed", nameFor (kFeed - 1));

    // ---- PERSISTENCE ------------------------------------------------------------------------------------
    // applySyncedIdentities wrote the cache; a machine that has synced once must start complete.
    {
        auto reopened = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
        reopened->loadFromDisk();
        reopened->buildRecommendable (scannerRows(), {});
        check (reopened->syncedIdentityCount() == kSynced,
               "D. the store survives a restart (param_maps.json)",
               juce::String (reopened->syncedIdentityCount()) + " identit(ies)");
        check (entryCount (reopened->buildMapFpsJson()) >= kSynced,
               "D. ...so the first turn after a restart carries them too",
               juce::String (entryCount (reopened->buildMapFpsJson())) + " entr(ies)");
    }

    // ---- REFUSALS UNCHANGED ------------------------------------------------------------------------------
    {
        juce::PluginDescription zero = descFor (7);
        zero.uniqueId = 0;
        check (echojay::syncedFpForIdentity ({ { echojay::identityKeyForDescription (zero),
                                                 { "SOMETHING", "1.0", "exact" } } }, zero).isEmpty(),
               "E. a zero uid is still no identity, synced or not");
        std::map<juce::String, echojay::SyncedIdentity> none;
        check (echojay::syncedFpForIdentity (none, descFor (3)).isEmpty(),
               "E. an empty store answers nothing rather than guessing");
    }

    // ---- F. THE WIRE SHAPE, PINNED AGAINST A VERBATIM LIVE BODY -------------------------------------------
    // Captured from the deployed endpoint on 23 Sep 2026. The server joins by "i" (the index into the batch that
    // was sent) and never echoes the identity key, so this is the one thing that must not drift silently.
    {
        const char* liveBody =
            R"({"count":4,"mapped":1,"unmapped":3,"tiers":{"exact":1,"same_count":0,"prefix":0,"none":3},)"
            R"("results":[{"i":0,"name":"Pro-Q 3","mapped":true,"tier":"exact",)"
            R"("fp":"e9ec80394bda9b4a893af34255f31b1de4b69a1434237458df37e5c2f7ddda94","version":"3.2.3",)"
            R"("versions":["3.2.5","3.2.3"]},)"
            R"({"i":1,"name":"Auto-Tune Pro","mapped":false,"tier":"none","fp":null,"version":null,"versions":[]},)"
            R"({"i":2,"name":"UAD Pultec HLF-3C","mapped":false,"tier":"none","fp":null,"version":null,"versions":[]},)"
            R"({"i":9,"name":"off the end","mapped":true,"tier":"exact","fp":"deadbeef","version":"1.0","versions":[]}],"ms":372})";
        const std::vector<echojay::SyncRef> sent {
            { "AudioUnit|61453c50|3.2.3",  "Pro-Q 3",           "FabFilter",       "AudioUnit", "3.2.3" },
            { "AudioUnit|76720177|10.5.0", "Auto-Tune Pro",     "Antares",         "AudioUnit", "10.5.0" },
            { "AudioUnit|12616669|11.8.0", "UAD Pultec HLF-3C", "Universal Audio", "AudioUnit", "11.8.0" } };
        int unjoinable = 0;
        const auto parsed = echojay::parseSyncResponse (juce::JSON::parse (liveBody), sent, &unjoinable);
        check (parsed.size() == 1, "F. the live body yields exactly the ONE mapped row",
               juce::String ((int) parsed.size()) + " of 4 rows");
        const auto it = parsed.find ("AudioUnit|61453c50|3.2.3");
        check (it != parsed.end() && it->second.fp == "e9ec80394bda9b4a893af34255f31b1de4b69a1434237458df37e5c2f7ddda94",
               "F. ...joined by \"i\" onto the identity key the CLIENT sent, carrying the server's fp",
               it == parsed.end() ? juce::String ("not stored") : it->second.fp.substring (0, 16) + "...");
        check (it != parsed.end() && it->second.version == "3.2.3" && it->second.tier == "exact",
               "F. ...with version and tier stored beside it",
               it == parsed.end() ? juce::String() : it->second.version + " / " + it->second.tier);
        check (parsed.count ("AudioUnit|76720177|10.5.0") == 0,
               "F. a mapped:false / null-fp row is not an answer and is not stored");
        check (unjoinable == 1, "F. a row whose index is off the end is DROPPED, never guessed at",
               juce::String (unjoinable) + " unjoinable");
    }

    std::printf ("\n==== mapfps_sync_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
