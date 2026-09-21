#pragma once
// EJPaceCheck (21 Sep 2026 ruling): an UNSIGNED harness must never load a PACE-wrapped bundle - PACE refuses to bootstrap
// in an unsigned host and puts a "Fatal error: 100001 - fatal wrapper bootstrap error" dialog on the user's desktop (fourteen of
// the sixty-six corpus plugins are wrapped; the 21 Sep index scan raised the dialogs two at a time, AU then VST3). The signed
// EchoJayProbe (Developer ID, hardened runtime, disable-library-validation) is the only harness process allowed to load them.
#define EJ_PACE_CHECK 1
#include <JuceHeader.h>
namespace echojay
{
// The bundle carries PACE: an Eden bundle, a PACE/Eden framework, or PACE symbols in its main binary.
inline bool isPaceWrapped (const juce::File& bundle)
{
    if (! bundle.isDirectory()) return false;
    const auto contents = bundle.getChildFile ("Contents");
    if (contents.getChildFile ("__Pace_Eden.bundle").exists() || contents.getChildFile ("Resources").getChildFile ("__Pace_Eden.bundle").exists()) return true;
    for (const auto& f : contents.getChildFile ("Frameworks").findChildFiles (juce::File::findFilesAndDirectories, false))
        if (f.getFileName().containsIgnoreCase ("pace") || f.getFileName().containsIgnoreCase ("eden")) return true;
    for (const auto& bin : contents.getChildFile ("MacOS").findChildFiles (juce::File::findFiles, false))
    {
        juce::MemoryBlock mb; if (! bin.loadFileAsData (mb)) continue;
        const juce::String hay (juce::CharPointer_UTF8 ((const char*) mb.getData()), (size_t) mb.getSize());
        juce::ignoreUnused (hay);
        const char* p = (const char*) mb.getData(); const size_t n = mb.getSize();
        for (const char* needle : { "PACEAntiPiracy", "com.paceap", "PaceAP", "__Pace_Eden" })
        {
            const size_t m = strlen (needle);
            for (size_t i = 0; i + m <= n; ++i) if (memcmp (p + i, needle, m) == 0) return true;
        }
        break;   // the main binary only
    }
    return false;
}
// The .component / .vst3 bundle a plugin description names: a VST3 carries its path; an AU is found by its AudioComponents name.
inline juce::File bundleFor (const juce::PluginDescription& d)
{
    if (d.pluginFormatName == "VST3") return juce::File (d.fileOrIdentifier);
    for (const juce::File dir : { juce::File ("/Library/Audio/Plug-Ins/Components"), juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/Components") })
        for (const auto& c : dir.findChildFiles (juce::File::findDirectories, false, "*.component"))
        {
            auto plist = juce::parseXML (c.getChildFile ("Contents/Info.plist"));
            if (plist == nullptr) continue;
            const auto text = plist->toString();
            if (text.contains ("<string>" + d.name + "</string>") || text.contains (": " + d.name + "</string>")) return c;
        }
    return {};
}
// The refusal an unsigned harness makes BEFORE instantiating: the reason, or empty when the plugin may load here.
inline juce::String refuseIfPaceWrapped (const juce::PluginDescription& d)
{
    const auto b = bundleFor (d);
    if (b != juce::File() && isPaceWrapped (b)) return "PACE-wrapped: signed probe only (" + b.getFileName() + ")";
    return {};
}
}
