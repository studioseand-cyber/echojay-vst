/*
  EchoJayBridgedAU.h

  Bridged-AU detection (10 Aug 2026, DEFECT_BRIDGED_READBACK option a).

  An Intel-only AU hosted from an arm64 process runs out-of-process through
  AUHostingServiceXPC, and a display read in the same stack frame as a write
  returns the PRE-write text -- so dial-time readback reverts correct writes.
  applyOne demotes display verification to norm round-trip on these
  instances; this header answers "is this instance one of them".

  MEASURED, NEVER INFERRED, AND UNKNOWN READS NATIVE. The verdict comes from
  the component bundle's executable architectures
  (CFBundleCopyExecutableArchitectures: reads Mach-O headers, loads no code).
  Two ways to find the bundle:

    1. EXACT: the bundle's Info.plist AudioComponents entry matching this
       component's type/subtype/manufacturer codes. Covers statically
       registered components (Melda, UAD, PA, ...).

    2. THE WAVES RULE, for the population the defect was measured on. Waves
       registers ~600 per-plugin components DYNAMICALLY from WaveShell, so
       no plist names them (measured 10 Aug: WaveShell1-AU 12.4 lists 3
       StudioRack shells, not APCM) -- but every ksWV component is served by
       a WaveShell binary, a distribution fact the scanner already relies on
       (PluginScanner skips manufacturer "Waves" from the registry walk for
       the same reason). So manufacturer ksWV takes the arch of the
       WaveShell*-AU bundles ACTUALLY ON DISK: bridged only when at least
       one exists and NONE carries arm64 (measured this machine: every
       readable shell is x86_64-only). A machine with any arm64 WaveShell
       reads native -- suppressing real reverts is the worse failure, per
       the option-a decision.

  Anything else -- no match, unreadable bundle, unparseable identifier, a
  non-AU format, an x86 process (no bridging exists there) -- reads NATIVE,
  so the demotion can never silently spread beyond what was measured.
*/

#pragma once

// CoreFoundation BEFORE any JUCE header in the including TU (the Codec
// Player rule: Apple frameworks first, or MacTypes' Point collides with
// juce::Point). ChainHost.cpp includes this file first for the same reason.
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <juce_audio_processors/juce_audio_processors.h>

namespace echojay
{

#if JUCE_MAC && defined (__aarch64__)

namespace bridgedDetail
{
    struct ComponentArchIndex
    {
        // "type,subtype,manu" -> bundle executable has arm64
        std::map<juce::String, bool> byCodes;
        bool anyWaveShell = false;
        bool anyWaveShellArm64 = false;
    };

    inline bool bundleHasArm64 (const juce::File& bundle)
    {
        auto url = CFURLCreateFromFileSystemRepresentation (
            nullptr, (const UInt8*) bundle.getFullPathName().toRawUTF8(),
            (CFIndex) bundle.getFullPathName().getNumBytesAsUTF8(), true);
        if (url == nullptr) return true;   // unreadable reads native
        auto* cfBundle = CFBundleCreate (nullptr, url);
        CFRelease (url);
        if (cfBundle == nullptr) return true;
        bool arm64 = false;
        if (auto* archs = CFBundleCopyExecutableArchitectures (cfBundle))
        {
            for (CFIndex i = 0; i < CFArrayGetCount (archs); ++i)
            {
                int v = 0;
                if (auto* num = (CFNumberRef) CFArrayGetValueAtIndex (archs, i))
                    CFNumberGetValue (num, kCFNumberIntType, &v);
                if (v == 0x0100000c /* kCFBundleExecutableArchitectureARM64 */) arm64 = true;
            }
            CFRelease (archs);
        }
        else
            arm64 = true;   // no readable executable: unknown reads native
        CFRelease (cfBundle);
        return arm64;
    }

    inline const ComponentArchIndex& componentArchIndex()
    {
        static const ComponentArchIndex idx = []
        {
            ComponentArchIndex out;
            const juce::File dirs[] = {
                juce::File ("/Library/Audio/Plug-Ins/Components"),
                juce::File ("~/Library/Audio/Plug-Ins/Components"),
            };
            for (const auto& dir : dirs)
                for (const auto& entry : juce::RangedDirectoryIterator (
                         dir, false, "*.component", juce::File::findDirectories))
                {
                    const auto bundle = entry.getFile();
                    const bool arm64 = bundleHasArm64 (bundle);

                    if (bundle.getFileName().startsWithIgnoreCase ("WaveShell"))
                    {
                        out.anyWaveShell = true;
                        out.anyWaveShellArm64 = out.anyWaveShellArm64 || arm64;
                    }

                    // Info.plist AudioComponents, read without loading code.
                    auto url = CFURLCreateFromFileSystemRepresentation (
                        nullptr, (const UInt8*) bundle.getFullPathName().toRawUTF8(),
                        (CFIndex) bundle.getFullPathName().getNumBytesAsUTF8(), true);
                    if (url == nullptr) continue;
                    auto* cfBundle = CFBundleCreate (nullptr, url);
                    CFRelease (url);
                    if (cfBundle == nullptr) continue;
                    auto components = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (
                        cfBundle, CFSTR ("AudioComponents"));
                    if (components != nullptr && CFGetTypeID (components) == CFArrayGetTypeID())
                        for (CFIndex i = 0; i < CFArrayGetCount (components); ++i)
                        {
                            auto entryDict = (CFDictionaryRef) CFArrayGetValueAtIndex (components, i);
                            if (entryDict == nullptr || CFGetTypeID (entryDict) != CFDictionaryGetTypeID())
                                continue;
                            auto str = [&entryDict] (CFStringRef key) -> juce::String
                            {
                                auto v = (CFStringRef) CFDictionaryGetValue (entryDict, key);
                                return (v != nullptr && CFGetTypeID (v) == CFStringGetTypeID())
                                         ? juce::String::fromCFString (v) : juce::String();
                            };
                            const auto key = str (CFSTR ("type")) + ","
                                           + str (CFSTR ("subtype")) + ","
                                           + str (CFSTR ("manufacturer"));
                            if (key.length() > 2 && out.byCodes.find (key) == out.byCodes.end())
                                out.byCodes[key] = arm64;
                        }
                    CFRelease (cfBundle);
                }
            return out;
        }();
        return idx;
    }
} // namespace bridgedDetail

/** True only when the component was MEASURED bridged: its serving binary
    carries no arm64 slice. See the header comment for the two lookup paths
    and the unknown-reads-native rule.
*/
inline bool auComponentIsBridged (const juce::PluginDescription& desc)
{
    if (! desc.pluginFormatName.equalsIgnoreCase ("AudioUnit")) return false;

    // "AudioUnit:Effects/aufx,APCM,ksWV" -> "aufx,APCM,ksWV"
    const auto id = desc.fileOrIdentifier.fromLastOccurrenceOf ("/", false, false);
    const auto codes = juce::StringArray::fromTokens (id, ",", "");
    if (codes.size() != 3) return false;

    const auto& idx = bridgedDetail::componentArchIndex();
    const auto key = codes[0] + "," + codes[1] + "," + codes[2];
    if (auto it = idx.byCodes.find (key); it != idx.byCodes.end())
        return ! it->second;

    if (codes[2] == "ksWV")
        return idx.anyWaveShell && ! idx.anyWaveShellArm64;

    return false;   // unknown reads native
}

#else

inline bool auComponentIsBridged (const juce::PluginDescription&) { return false; }

#endif

} // namespace echojay
