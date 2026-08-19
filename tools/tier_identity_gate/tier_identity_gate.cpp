/*
  tier_identity_gate: the pin under the whole P16 catalogue. The catalogue is
  only worth anything if the fingerprint ejextract's worker writes for a plugin
  is byte-for-byte the fingerprint the plugin computes when it loads. If those
  two enumerations ever fork one field, every seeded map silently misses and
  nothing anywhere says so, and 773 dialable becomes 773 keys that miss.

  So this fails LOUDLY. For each plugin it computes the fingerprint two ways:

    in-process:  findAllTypesForFile, createPluginInstance, then
                 fingerprintForDescription(inst->getPluginDescription(),
                 inst->getParameters().size()) - EXACTLY the two sources
                 ChainHost::completeLoad fingerprints from.
    worker:      spawns `ejextract --id-worker <target>` and reads the fp it
                 emitted.

  and asserts they are equal. Two plugins of a different format AND vendor
  (FabFilter Pro-Q 3, a VST3, and Apple's stock Dynamics Processor, an AU) so
  a pass is not one lucky bundle.

  Built by build_and_run.sh, which lifts the real compile flags from
  build/compile_commands.json and links libEchoJay V2_SharedCode.a for the JUCE
  symbols, so this TU computes the fingerprint with the SAME shipped code the
  plugin does. EchoJayParamMaps.h is header-only inline: including it IS the
  shipped implementation.

  House style: no em-dashes.
*/
#include <JuceHeader.h>
#include "EchoJayParamMaps.h"
#include <iostream>

static int passN = 0, failN = 0;
static void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    if (ok) { ++passN; std::cout << "  ok    " << name << "\n"; }
    else    { ++failN; std::cout << "  FAIL  " << name
                                 << (detail.isNotEmpty() ? ("\n        " + detail) : juce::String()) << "\n"; }
}

// The in-process side: the fingerprint the plugin computes when it loads.
static juce::String inProcessFp (juce::AudioPluginFormatManager& fm, const juce::String& target,
                                 juce::String& whyEmpty)
{
    juce::OwnedArray<juce::PluginDescription> found;
    for (int i = 0; i < fm.getNumFormats(); ++i)
        fm.getFormat (i)->findAllTypesForFile (found, target);
    if (found.isEmpty()) { whyEmpty = "no types enumerated"; return {}; }

    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> inst (
        fm.createPluginInstance (*found[0], 48000.0, 512, error));
    if (inst == nullptr) { whyEmpty = "instantiate failed: " + error; return {}; }

    auto live = inst->getPluginDescription();
    if (live.pluginFormatName.isEmpty() || live.version.isEmpty()) live = *found[0];
    return echojay::fingerprintForDescription (live, inst->getParameters().size());
}

// The worker side: the fingerprint ejextract emits for the same target.
static juce::String workerFp (const juce::String& ejextract, const juce::String& target,
                              juce::String& whyEmpty)
{
    auto wd = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("gate_" + juce::String (juce::Time::getHighResolutionTicks()));
    wd.createDirectory();
    juce::ChildProcess p;
    if (! p.start (juce::StringArray { ejextract, "--id-worker", target, wd.getFullPathName() }))
    { whyEmpty = "could not spawn ejextract"; wd.deleteRecursively(); return {}; }
    p.waitForProcessToFinish (150000);

    auto j = juce::JSON::parse (wd.getChildFile ("id.json").loadFileAsString());
    juce::String fp;
    if (auto* arr = j.getProperty ("effects", juce::var()).getArray())
        if (! arr->isEmpty()) fp = (*arr)[0].getProperty ("fp", juce::var()).toString();
    if (fp.isEmpty()) whyEmpty = "worker emitted no fp (id.json effects empty)";
    wd.deleteRecursively();
    return fp;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 2)
    {
        std::cerr << "usage: tier_identity_gate <path-to-ejextract> [<label=target> ...]\n";
        return 2;
    }
    const juce::String ejextract = juce::String (juce::CharPointer_UTF8 (argv[1]));

    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::VST3PluginFormat());
#if JUCE_MAC
    fm.addFormat (new juce::AudioUnitPluginFormat());
#endif

    // Default cases: a VST3 and an AU, different vendors. Overridable on argv
    // as label=target pairs so the rig can point at whatever is installed.
    juce::StringPairArray cases;
    if (argc > 2)
        for (int i = 2; i < argc; ++i)
        {
            const juce::String a = juce::String (juce::CharPointer_UTF8 (argv[i]));
            cases.set (a.upToFirstOccurrenceOf ("=", false, false),
                       a.fromFirstOccurrenceOf ("=", false, false));
        }
    else
    {
        cases.set ("ProQ3-VST3-FabFilter", "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-Q 3.vst3");
        cases.set ("Dynamics-AU-Apple",    "AudioUnit:Effects/aufx,dcmp,appl");
    }

    std::cout << "tier_identity_gate: worker fp == in-process fp\n";
    for (const auto& label : cases.getAllKeys())
    {
        const auto target = cases[label];
        juce::String whyA, whyB;
        const auto a = inProcessFp (fm, target, whyA);
        const auto b = workerFp (ejextract, target, whyB);
        if (a.isEmpty() || b.isEmpty())
            check (false, label, "in-process=" + (a.isEmpty() ? "EMPTY (" + whyA + ")" : a.substring (0, 16))
                               + "  worker=" + (b.isEmpty() ? "EMPTY (" + whyB + ")" : b.substring (0, 16)));
        else
            check (a == b, label, a == b ? juce::String()
                                         : "in-process=" + a + "\n        worker=     " + b);
    }

    std::cout << (failN == 0 ? "\nALL PASS\n" : "\nFAILED\n");
    return failN == 0 ? 0 : 1;
}
