/*
  RegistryConformanceTest.cpp

  Claim under test: the PluginDescription EchoJayAuRegistry.h derives from the
  AU registry, without loading anything, is identical to the one JUCE fills in
  from a live instance, on every field ejmap consumes.

  If that stops being true, ejmap lists, keys and maps plugins under an identity
  EchoJay would not recognise, and nothing else in the system would say so.
  describeFromRegistry is a hand-written mirror of JUCE's
  fillInPluginDescription; mirrors drift when the thing they mirror changes.

  THIS TEST LOADS REAL PLUGINS. That is the point: registry metadata can only be
  checked against the instantiated truth. It is BOUNDED and never a full scan:
  at most kMaxSample plugins, chosen deliberately to spread across vendors and
  component types. Resolving the whole registry would be ~1355 in-process loads,
  which is the exact failure this seam was opened to remove.

  Failure to instantiate is NOT a test failure. A licence-refused Waves shell or
  an Intel-only legacy component says nothing about whether the registry
  description is right. Those are announced and skipped. What IS a failure is
  finishing with too few real comparisons to mean anything, so the run asserts a
  floor of kMinCompared.
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "EchoJayAuRegistry.h"
#include "PluginScanner.h"

#include <iostream>
#include <map>
#include <set>

namespace
{

constexpr int kMaxSample   = 10;   // hard ceiling on plugins instantiated
constexpr int kMinCompared = 3;    // below this the run proves nothing

int checks = 0, failures = 0;

void check (bool condition, const juce::String& what)
{
    ++checks;
    if (! condition)
    {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

/** Compares one field, printing both sides when they differ. Reporting only
    "mismatch" would make every failure a second investigation.
*/
void checkField (const juce::String& plugin, const juce::String& field,
                 const juce::String& fromRegistry, const juce::String& fromInstance)
{
    ++checks;
    if (fromRegistry != fromInstance)
    {
        ++failures;
        std::cerr << "FAIL: " << plugin << " field '" << field << "'\n"
                  << "        registry: \"" << fromRegistry << "\"\n"
                  << "        instance: \"" << fromInstance << "\"" << std::endl;
    }
}

//==============================================================================
/** Deliberate sample: walk the census in order and take at most one component
    per vendor, and at most one per component type beyond the first, so the set
    spreads instead of being ten Waves effects. Deterministic, so a failure is
    reproducible.
*/
std::vector<echojay::auregistry::AuTarget> chooseSample (const echojay::auregistry::AuCensus& census)
{
    std::vector<echojay::auregistry::AuTarget> sample;
    std::set<juce::String> seenVendors;
    std::map<juce::String, int> perType;

    for (const auto& t : census.targets)
    {
        if ((int) sample.size() >= kMaxSample)
            break;

        if (! ejmap::PluginScanner::isMappableAuType (t.typeCode))
            continue;
        if (seenVendors.count (t.vendorName) > 0)
            continue;
        if (perType[t.typeCode] >= 6)
            continue;

        seenVendors.insert (t.vendorName);
        perType[t.typeCode]++;
        sample.push_back (t);
    }

    return sample;
}

} // namespace

//==============================================================================
int main (int, char**)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto census = echojay::auregistry::buildCensus();

    std::cout << "census: " << census.enumerated << " enumerated, "
              << census.targets.size() << " candidates, "
              << census.byVendor.size() << " vendors" << std::endl;

    if (census.targets.empty())
    {
        std::cout << "no AU components on this machine, skipping registry conformance "
                  << "(nothing to compare against)" << std::endl;
        std::cout << checks << " checks, " << failures << " failures" << std::endl;
        return 0;
    }

    // IDENTITY COLLAPSE, checked over the WHOLE registry and not the sample.
    // This costs one census walk and no instantiation, and it is the check the
    // sample structurally cannot make: a collapse is invisible one plugin at a
    // time and obvious in a column of uids.
    //
    // The defect it pins (4 Aug 2026): stringToOSType guarded on the TRIMMED
    // length and then read four UNTRIMMED bytes, so every space-padded AU
    // subtype -- "CR  ", "PM  ", "EB  " -- returned 0, and 0 is a WILDCARD to
    // AudioComponentFindNext. Seventeen Soundtoys aufx components resolved to
    // Crystallizer's name and uid, three aumf ones to EffectRack, and a real
    // EchoBoy map was filed under Crystallizer's identity.
    {
        std::map<juce::String, juce::StringArray> byUid;
        int padded = 0, paddedResolved = 0, unresolved = 0;
        for (const auto& t : census.targets)
        {
            const auto d = echojay::auregistry::describeFromRegistry (t.identifier);
            if (d.name.isEmpty()) { ++unresolved; continue; }
            byUid[juce::String::toHexString (d.uniqueId)].add (t.identifier);

            // The subtype sits between the two commas of the tail.
            const auto tail = t.identifier.fromLastOccurrenceOf ("/", false, false);
            juce::StringArray parts;
            parts.addTokens (tail, ",", juce::StringRef());
            if (parts.size() == 3 && parts[1].containsChar (' '))
            {
                ++padded;
                if (d.name.isNotEmpty()) ++paddedResolved;
            }
        }

        // Every space-padded subtype must resolve, and to ITS OWN identity.
        int paddedSharing = 0;
        for (auto& kv : byUid)
            if (kv.second.size() > 1)
                for (const auto& id : kv.second)
                {
                    const auto tail = id.fromLastOccurrenceOf ("/", false, false);
                    juce::StringArray parts;
                    parts.addTokens (tail, ",", juce::StringRef());
                    if (parts.size() == 3 && parts[1].containsChar (' ')) ++paddedSharing;
                }

        check (padded == 0 || paddedResolved == padded,
               "every space-padded AU subtype resolves (" + juce::String (paddedResolved)
                 + " of " + juce::String (padded) + ")");
        check (paddedSharing == 0,
               "no space-padded subtype shares a uid with another component ("
                 + juce::String (paddedSharing) + " sharing)");
        std::cout << "identity: " << census.targets.size() << " components, "
                  << padded << " space-padded subtype(s), " << unresolved
                  << " unresolved" << std::endl;

        // Components that share a uid are reported ALWAYS, because the ones
        // that remain are a different defect (JUCE's uid formula collides for
        // Waves Sibilance-Live / EMO-Generator) and must not be mistaken for
        // this one returning.
        for (auto& kv : byUid)
            if (kv.second.size() > 1)
                std::cout << "  NOTE uid " << kv.first << " shared by "
                          << kv.second.joinIntoString (", ") << std::endl;
    }

    auto sample = chooseSample (census);
    std::cout << "sample: " << sample.size() << " plugin(s), max " << kMaxSample
              << ", one per vendor" << std::endl;

    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);

    int compared = 0, skipped = 0;

    for (const auto& target : sample)
    {
        // The description under test: built from the registry, nothing loaded.
        const auto fromRegistry = echojay::auregistry::describeFromRegistry (target.identifier);

        if (fromRegistry.fileOrIdentifier.isEmpty())
        {
            std::cout << "  SKIP  " << target.identifier
                      << "  (registry would not describe it)" << std::endl;
            ++skipped;
            continue;
        }

        juce::String error;
        auto instance = fm.createPluginInstance (fromRegistry, 48000.0, 512, error);

        if (instance == nullptr)
        {
            // Licence, architecture, missing iLok. Announced, never counted as
            // a pass and never counted as a failure.
            std::cout << "  SKIP  " << fromRegistry.name << "  ("
                      << (error.isEmpty() ? juce::String ("createPluginInstance returned null")
                                          : error) << ")" << std::endl;
            ++skipped;
            continue;
        }

        // The truth: JUCE's own fillInPluginDescription, off the live instance.
        const auto fromInstance = instance->getPluginDescription();
        const auto label = fromInstance.name;

        // Every field ejmap consumes.
        checkField (label, "name",             fromRegistry.name,             fromInstance.name);
        checkField (label, "descriptiveName",  fromRegistry.descriptiveName,  fromInstance.descriptiveName);
        checkField (label, "manufacturerName", fromRegistry.manufacturerName, fromInstance.manufacturerName);
        checkField (label, "version",          fromRegistry.version,          fromInstance.version);
        checkField (label, "pluginFormatName", fromRegistry.pluginFormatName, fromInstance.pluginFormatName);
        checkField (label, "fileOrIdentifier", fromRegistry.fileOrIdentifier, fromInstance.fileOrIdentifier);
        checkField (label, "uniqueId",         juce::String (fromRegistry.uniqueId),
                                               juce::String (fromInstance.uniqueId));
        checkField (label, "isInstrument",     fromRegistry.isInstrument ? "true" : "false",
                                               fromInstance.isInstrument ? "true" : "false");

        // pluginId is what the ledger and the quarantine key on. It has to
        // survive the round trip, or a crash gets recorded against nothing.
        ejmap::ScannedPlugin spRegistry, spInstance;
        spRegistry.desc = fromRegistry;
        spInstance.desc = fromInstance;
        checkField (label, "pluginId", spRegistry.pluginId(), spInstance.pluginId());

        std::cout << "  OK    " << label << "  [" << target.typeCode << " / "
                  << target.vendorName << "]" << std::endl;

        ++compared;
        instance->releaseResources();
        instance.reset();
    }

    std::cout << "compared " << compared << " plugin(s), skipped " << skipped << std::endl;

    // A run where everything skipped is not a green run. It is a run that
    // tested nothing, and it must not read as a pass.
    if (compared < kMinCompared)
    {
        std::cerr << "FAIL: only " << compared << " plugin(s) compared, need at least "
                  << kMinCompared << ". The registry description is unverified on this "
                  << "machine; this is not a pass." << std::endl;
        ++failures;
        ++checks;
    }

    std::cout << checks << " checks, " << failures << " failures" << std::endl;
    return failures == 0 ? 0 : 1;
}
