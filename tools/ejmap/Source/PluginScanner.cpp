#include "PluginScanner.h"
#include "EchoJayAuRegistry.h"   // the one AU walk, shared with ejextract

namespace ejmap
{

PluginScanner::PluginScanner()
{
    // JUCE 8.0.12 deleted AudioPluginFormatManager::addDefaultFormats(). The
    // free function is what ChainHost.cpp uses, so ejmap registers the same
    // format set the plugin does.
    juce::addDefaultFormatsToManager (formatManager);
}

//==============================================================================
void PluginScanner::scanAudioUnits (Result& result)
{
    // One walk, shared with ejextract. JUCE's component type filter and its
    // AUv3 exclusion are taken wholesale: deviating from them is exactly how
    // this tree ended up with four different filters.
    const auto census = echojay::auregistry::buildCensus();

    result.auEnumerated    = census.enumerated;
    result.auExcludedApple = census.excludedApple;
    result.auExcludedEcho  = census.excludedEcho;
    result.auUnparsed      = census.unparsed;

    for (const auto& target : census.targets)
    {
        ++result.totalFound;

        // Registry metadata only. No plugin code runs here.
        auto desc = echojay::auregistry::describeFromRegistry (target.identifier);

        if (desc.fileOrIdentifier.isEmpty())
        {
            // A registered component the registry would not describe. Recorded,
            // not dropped in silence: this is the class of thing the old file
            // walk discarded without a word.
            result.errors.add ("no registry description for AU component " + target.identifier);
            ++result.droppedByType[target.typeCode.isEmpty() ? juce::String ("(unparsed)") : target.typeCode];
            continue;
        }

        // Both drops below are counted by type. The human can see that 52 aumu
        // and 12 augn/aumi went, rather than seeing a list that is quietly 64
        // entries shorter than the walk.
        if (desc.isInstrument)
        {
            ++result.instrumentsFiltered;
            ++result.droppedByType[target.typeCode];
            continue;
        }

        if (! isMappableAuType (target.typeCode))
        {
            ++result.unusedTypeFiltered;
            ++result.droppedByType[target.typeCode];
            continue;
        }

        ScannedPlugin sp;
        sp.desc = desc;
        result.plugins.add (sp);
    }
}

//==============================================================================
void PluginScanner::scanVST3 (Result& result, Ledger& ledger, Watchdog& watchdog)
{
    juce::AudioPluginFormat* vst3 = nullptr;
    for (auto* f : formatManager.getFormats())
        if (f->getName() == "VST3")
            vst3 = f;

    if (vst3 == nullptr)
    {
        result.errors.add ("VST3 format not registered in the format manager");
        return;
    }

    auto paths = vst3->getDefaultLocationsToSearch();
    auto files = vst3->searchPathsForPlugins (paths, true, true);

    for (const auto& file : files)
    {
        ++result.totalFound;

        // A bundle that killed a previous Scan is not probed again. Without
        // this, recoverFromCrash quarantines it and the very next Scan walks
        // back into it: relaunch, crash, relaunch, forever. Quarantine has to
        // gate the scan path for the same reason it gates the load path.
        if (ledger.isQuarantined (file))
        {
            ++result.vst3Quarantined;
            result.errors.add ("skipped quarantined VST3 " + file
                                 + " (a previous scan died inside it; release it explicitly to retry)");
            continue;
        }

        // findAllTypesForFile is banned on the AU path because AU's version
        // unconditionally instantiates. VST3's version does not: it tries
        // DescriptionLister::findDescriptionsFast first, which reads
        // moduleinfo.json and loads no code, and only falls back to opening the
        // module for bundles that predate moduleinfo. There is also no VST3
        // registry to enumerate instead, so this is the only mechanism. See
        // juce_VST3PluginFormatHeadless.cpp:54.
        //
        // That fallback is not rare: 646 of 860 bundles on the development
        // machine ship no moduleinfo.json. So this call runs plugin code most
        // of the time, and it gets the same inflight protocol the load path
        // uses. Keyed on the file path, because there is no description yet to
        // take a name or a uid from.
        const auto probeId = file;
        const auto probeName = juce::File (file).getFileNameWithoutExtension();

        ledger.beginLoad (probeId, probeName, /*vendor*/ {}, "VST3", /*version*/ {},
                          "scan", "findAllTypesForFile");

        juce::OwnedArray<juce::PluginDescription> found;
        {
            // bloom.vst3 hangs here past 90 s, and hangs ejextract's isolated
            // worker in the same call. Without a deadline this wedges the whole
            // scan with inflight.json on disk and the app alive.
            Watchdog::Scope guard (watchdog, "findAllTypesForFile", probeId, probeName,
                                   "VST3", "scan");
            vst3->findAllTypesForFile (found, file);
        }

        ++result.vst3Probed;

        LedgerRecord rec;
        rec.pluginId = probeId;
        rec.name     = juce::File (file).getFileNameWithoutExtension();
        rec.format   = "VST3";
        rec.stage    = "scan";
        rec.outcome  = found.isEmpty() ? LoadOutcome::noTypes : LoadOutcome::ok;
        rec.detail   = found.isEmpty()
                         ? "format opened the file and returned no descriptions"
                         : juce::String (found.size()) + " description(s)";

        if (! found.isEmpty())
        {
            rec.vendor  = found[0]->manufacturerName;
            rec.version = found[0]->version;
        }

        ledger.endLoad (rec);

        if (found.isEmpty())
        {
            result.errors.add ("no description for VST3 " + file);
            continue;
        }

        for (auto* pd : found)
        {
            if (pd->isInstrument)
            {
                ++result.instrumentsFiltered;
                ++result.droppedByType["vst3-instrument"];
                continue;
            }

            ScannedPlugin sp;
            sp.desc = *pd;
            result.plugins.add (sp);
        }
    }
}

//==============================================================================
PluginScanner::Result PluginScanner::scan (Ledger& ledger, Watchdog& watchdog)
{
    Result result;

    // AU needs no inflight record: the census reads registry metadata and never
    // hands control to plugin code. VST3 does, so it takes the Ledger.
    scanAudioUnits (result);
    scanVST3 (result, ledger, watchdog);

    juce::StringArray distinct;
    for (const auto& p : result.plugins)
        distinct.addIfNotAlreadyThere (p.pluginId());

    result.distinctProducts = distinct.size();
    return result;
}

} // namespace ejmap
