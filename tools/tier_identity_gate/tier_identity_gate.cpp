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

static juce::String fpOfDescription (juce::AudioPluginFormatManager& fm,
                                     const juce::PluginDescription& d, juce::String& whyEmpty)
{
    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> inst (
        fm.createPluginInstance (d, 48000.0, 512, error));
    if (inst == nullptr) { whyEmpty = "instantiate failed: " + error; return {}; }
    // PREPARE before fingerprinting, exactly as ChainHost::completeLoad does
    // (it prepares the graph, then fingerprints). The worker does NOT prepare,
    // so this side computes the prepared param_count and the worker the
    // unprepared one: if a plugin's count moved on prepare, the two fps would
    // differ and this gate would catch it. Today none do, which is the point.
    inst->setPlayConfigDetails (2, 2, 48000.0, 512);
    inst->prepareToPlay (48000.0, 512);
    auto live = inst->getPluginDescription();
    if (live.pluginFormatName.isEmpty() || live.version.isEmpty()) live = d;
    return echojay::fingerprintForDescription (live, inst->getParameters().size());
}

// The in-process side: the fingerprint the plugin computes when it loads. A
// ".xml" target is a SAVED description (the shell sub-plugin case, where the
// identity comes from a stored enumeration rather than a fresh file scan);
// anything else is a bundle path or AU identifier to enumerate.
static juce::String inProcessFp (juce::AudioPluginFormatManager& fm, const juce::String& target,
                                 juce::String& whyEmpty)
{
    if (target.endsWithIgnoreCase (".xml"))
    {
        auto xml = juce::XmlDocument::parse (juce::File (target));
        juce::PluginDescription d;
        if (xml == nullptr || ! d.loadFromXml (*xml)) { whyEmpty = "saved desc did not load"; return {}; }
        return fpOfDescription (fm, d, whyEmpty);
    }
    juce::OwnedArray<juce::PluginDescription> found;
    for (int i = 0; i < fm.getNumFormats(); ++i)
        fm.getFormat (i)->findAllTypesForFile (found, target);
    if (found.isEmpty()) { whyEmpty = "no types enumerated"; return {}; }
    return fpOfDescription (fm, *found[0], whyEmpty);
}

// The worker side: the fingerprint ejextract emits for the same target.
static juce::String workerFp (const juce::String& ejextract, const juce::String& target,
                              juce::String& whyEmpty)
{
    auto wd = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("gate_" + juce::String (juce::Time::getHighResolutionTicks()));
    wd.createDirectory();
    const juce::String mode = target.endsWithIgnoreCase (".xml") ? "--instantiate-desc" : "--id-worker";
    juce::ChildProcess p;
    if (! p.start (juce::StringArray { ejextract, mode, target, wd.getFullPathName() }))
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

// Read the first effect object from an ejextract id.json.
static juce::var firstEffect (const juce::File& dir)
{
    auto j = juce::JSON::parse (dir.getChildFile ("id.json").loadFileAsString());
    if (auto* arr = j.getProperty ("effects", juce::var()).getArray())
        if (! arr->isEmpty()) return (*arr)[0];
    return {};
}

// The hard case, run entirely under Rosetta because the whole Waves library is
// x86-only: a shell sub-plugin's identity comes from a STORED shell enumeration
// (--enumerate-only), not a fresh file scan, and the catalogue keys the map on
// that stored key while the DAW looks it up from the LIVE instance. If those
// two keys fork, every Waves map misses. So: enumerate the shell, take one
// sub-plugin's saved description, instantiate it, and assert the key from the
// saved description equals the key from the live instance, and that a
// fingerprint was produced. It fails LOUDLY if it cannot even enumerate, so the
// gate is never silently green on the case that matters most.
static void shellForkCase (const juce::String& ejextract, const juce::String& shellPath)
{
    const juce::String label = "ShellSub-VST3-Waves (Rosetta)";
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getChildFile ("gate_shell_" + juce::String (juce::Time::getHighResolutionTicks()));
    auto wdEnum = tmp.getChildFile ("enum"); wdEnum.createDirectory();
    auto wdInst = tmp.getChildFile ("inst"); wdInst.createDirectory();

    auto runX86 = [&] (const juce::StringArray& extra, const juce::File& wd) -> bool
    {
        juce::StringArray a { "/usr/bin/arch", "-x86_64", ejextract };
        a.addArray (extra);
        a.add (wd.getFullPathName());
        juce::ChildProcess p;
        if (! p.start (a)) return false;
        p.waitForProcessToFinish (150000);
        return true;
    };

    if (! runX86 ({ "--enumerate-only", shellPath }, wdEnum))
    { check (false, label, "could not spawn ejextract under Rosetta"); tmp.deleteRecursively(); return; }

    auto eff = firstEffect (wdEnum);
    const auto desc = eff.getProperty ("desc", juce::var()).toString();
    if (desc.isEmpty())
    { check (false, label, "shell enumerated no sub-plugins at " + shellPath + " (present? x86 slice?)"); tmp.deleteRecursively(); return; }

    auto descFile = tmp.getChildFile ("sub.xml");
    descFile.replaceWithText (desc);
    if (! runX86 ({ "--instantiate-desc", descFile.getFullPathName() }, wdInst))
    { check (false, label, "could not instantiate the saved sub-plugin under Rosetta"); tmp.deleteRecursively(); return; }

    auto ie = firstEffect (wdInst);
    const auto ikSaved = ie.getProperty ("ikSaved", juce::var()).toString();
    const auto ikLive  = ie.getProperty ("ikLive",  juce::var()).toString();
    const auto fp      = ie.getProperty ("fp",      juce::var()).toString();
    if (ikSaved.isEmpty() || fp.isEmpty())
        check (false, label, "instantiate produced no key/fp (ikSaved=" + ikSaved + " fp=" + fp.substring (0, 12) + ")");
    else
        check (ikSaved == ikLive, label, ikSaved == ikLive
               ? juce::String()
               : "saved key=" + ikSaved + "  live key=" + ikLive + " (the catalogue and the DAW would fork)");
    tmp.deleteRecursively();
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
        // A REAL third-party AU with a LARGE parameter count (358), so a fp
        // computed from a zero or wrong count cannot pass unnoticed. Apple's
        // Dynamics stays as a small smoke case; it is not the pin.
        cases.set ("ProQ3-AU-FabFilter",   "/Library/Audio/Plug-Ins/Components/FabFilter Pro-Q 3.component");
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

    // The shell sub-plugin case (Waves), run under Rosetta since the library is
    // x86-only. Skipped only if no Waves shell is installed, and it says so.
    {
        const char* shells[] = {
            "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 12.6.vst3",
            "/Library/Audio/Plug-Ins/VST3/WaveShell-VST3 9.6.vst3" };
        juce::String present;
        for (auto* s : shells) if (juce::File (s).exists()) { present = s; break; }
        if (present.isNotEmpty()) shellForkCase (ejextract, present);
        else std::cout << "  n/a   ShellSub-VST3-Waves (no Waves shell installed)\n";
    }

    std::cout << (failN == 0 ? "\nALL PASS\n" : "\nFAILED\n");
    return failN == 0 ? 0 : 1;
}
