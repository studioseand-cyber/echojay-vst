/*
  PluginScanner.h

  Enumerates installed plugins.

  The AU walk is NOT here. It lives in tools/ejextract/EchoJayAuRegistry.h and is
  shared with ejextract, because this tree had grown three separate walks with
  four different component type filters, and enumeration drift is how the Waves
  shells went invisible. scanAudioUnits calls buildCensus and applies ejmap's
  policy on top of it, in the open.

  Nothing in the AU path instantiates a plugin. findAllTypesForFile is banned on
  that path: in JUCE 8.0.12 it calls createInstanceFromDescription, so resolving
  the registry would load ~1355 plugins in-process from one button click, and it
  silently returns empty for anything needing an unblocked message thread, which
  a Scan click is not.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>

#include "EjmapLedger.h"
#include "EjmapWatchdog.h"

namespace ejmap
{

struct ScannedPlugin
{
    juce::PluginDescription desc;

    /** The plugin's stable key: the format's own identifier string.

        AU: "AudioUnit:Effects/aufx,SILM,ksWV". VST3: the bundle path.

        NOT format:uniqueId. JUCE derives an AU uniqueId as
        componentType ^ componentSubType ^ componentManufacturer, and that XOR
        is not unique. Measured on this machine's 1419-component registry: 2
        collisions across 4 Waves components, where Sibilance-Live and
        EMO-Generator share a uid in both their mono and stereo forms.

        The ledger and the quarantine both key on this. Under the uid key,
        quarantining Sibilance-Live would silently quarantine EMO-Generator too.
        Version stays metadata and lives in desc, never in this id.
    */
    juce::String pluginId() const
    {
        return desc.fileOrIdentifier;
    }

    bool isInstrument() const { return desc.isInstrument; }
};

//==============================================================================
class PluginScanner
{
public:
    PluginScanner();

    struct Result
    {
        juce::Array<ScannedPlugin> plugins;      // mappable effects only

        int totalFound          = 0;             // candidates considered, all formats
        int instrumentsFiltered = 0;
        int unusedTypeFiltered  = 0;             // AU types ejmap does not map
        int distinctProducts    = 0;             // distinct pluginId

        // Straight off the shared census, so the scan can report what the walk
        // saw rather than only what survived it.
        int auEnumerated    = 0;
        int auExcludedApple = 0;
        int auExcludedEcho  = 0;
        int auUnparsed      = 0;

        int vst3Probed      = 0;   // bundles handed to the format
        int vst3Quarantined = 0;   // skipped: a previous Scan died inside them

        /** Component type code -> how many were dropped. Every drop lands here.
            A filter that discards without counting is the silent-drop class.
        */
        std::map<juce::String, int> droppedByType;

        juce::StringArray errors;                // never silent: every skip is logged
    };

    /** Walks the AU component registry and the VST3 search paths.

        The Ledger is REQUIRED, not optional. The VST3 leg hands each bundle to
        the format, which for any bundle without a moduleinfo.json opens the
        module and reads its factory: plugin code, inside a Scan, in-process.
        Without the inflight protocol around it a crash there leaves no record
        of which file did it, and the next Scan walks straight back into it.

        Passing a Ledger is therefore not a caller's choice. There is no
        overload that scans without one.
    */
    Result scan (Ledger& ledger, Watchdog& watchdog);

    juce::AudioPluginFormatManager& getFormatManager() noexcept { return formatManager; }

    /** AU component types ejmap maps: plain effects and music effects. Public so
        the reason a type was dropped is inspectable rather than buried in a
        conditional.
    */
    static bool isMappableAuType (const juce::String& typeCode) noexcept
    {
        return typeCode == "aufx"    // Effect
            || typeCode == "aumf"    // MusicEffect
            || typeCode == "aumx"    // Mixer
            || typeCode == "aupn";   // Panner
    }

private:
    void scanAudioUnits (Result&);
    void scanVST3 (Result&, Ledger&, Watchdog&);

    juce::AudioPluginFormatManager formatManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScanner)
};

} // namespace ejmap
