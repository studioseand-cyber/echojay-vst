// AUEnumerator.mm
// Compiled as a separate translation unit with NO JUCE headers included.
// This avoids the conflict between CoreAudio's "typedef struct AudioBuffer AudioBuffer"
// and juce::AudioBuffer<T> that appears in global scope via "using namespace juce;"
// in the generated JuceHeader.h.
//
// CoreAudio APIs used here are pure C — no dylib loading, no plugin instantiation.

#ifdef __APPLE__
 #include <AudioUnit/AudioUnit.h>       // kAudioUnitType_* constants
 #include <AudioToolbox/AudioComponent.h>
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include "AUEnumerator.h"
#include <cstring>

#ifdef __APPLE__

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <unordered_map>

static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    return s.substr(start, end - start + 1);
}

static std::string osTypeToStd(OSType t)
{
    char s[5] = {
        (char)((t >> 24) & 0xff),
        (char)((t >> 16) & 0xff),
        (char)((t >>  8) & 0xff),
        (char)( t        & 0xff),
        '\0'
    };
    return std::string(s, 4);
}

static std::string buildAUIdentifier(const AudioComponentDescription& d)
{
    std::string prefix = "AudioUnit:";
    if      (d.componentType == kAudioUnitType_MusicDevice)  prefix += "Synths/";
    else if (d.componentType == kAudioUnitType_MusicEffect
          || d.componentType == kAudioUnitType_Effect)       prefix += "Effects/";
    else if (d.componentType == kAudioUnitType_Generator)    prefix += "Generators/";
    else if (d.componentType == kAudioUnitType_MIDIProcessor) prefix += "MidiEffects/";
    else                                                      prefix += "Other/";

    prefix += osTypeToStd(d.componentType)        + ","
           + osTypeToStd(d.componentSubType)      + ","
           + osTypeToStd(d.componentManufacturer);
    return prefix;
}

static std::string cfStringToStd(CFStringRef cf)
{
    if (!cf) return {};
    const char* cptr = CFStringGetCStringPtr(cf, kCFStringEncodingUTF8);
    if (cptr) return std::string(cptr);
    CFIndex len    = CFStringGetLength(cf);
    CFIndex maxLen = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string buf(static_cast<size_t>(maxLen), '\0');
    if (CFStringGetCString(cf, &buf[0], maxLen, kCFStringEncodingUTF8))
        buf.resize(std::strlen(buf.c_str()));
    else
        buf.clear();
    return buf;
}

// ---------------------------------------------------------------------------
// Real versions from Info.plist (12 Aug 2026). The triplet in `version` made
// the identity key format|uid|version unmatchable against the live-load
// index (real versions), so exact=0 on every turn and the uid fallback
// carried the whole exposure - a fallback that goes AMBIGUOUS and refuses
// the first time a machine holds two fingerprints for one uid, i.e. it
// degrades with machine age. An AU component bundle's Info.plist carries an
// AudioComponents array with the triple plus a version integer, and decoding
// it as major/minor/patch reproduces exactly what a live load reports
// (measured: 766 bundles parsed in 325ms, all 92 uid-matched identities
// recovered their indexed version, zero mismatches).
//
// CFBundleCopyInfoDictionaryInDirectory reads the plist WITHOUT loading any
// code, preserving this TU's founding contract. AudioComponentGetVersion was
// considered and REJECTED: on legacy component registrations it can call
// into the component, i.e. load its code during the scan, which is exactly
// what this enumerator exists to never do.
//
// A component with no plist entry for its triple keeps the triplet and falls
// through to the uid path downstream - zero such components on the measured
// machine, but never an error.
static void harvestPlistVersions (const std::string& dirPath,
                                  std::unordered_map<std::string, std::string>& out)
{
    DIR* dir = opendir(dirPath.c_str());
    if (dir == nullptr) return;
    while (auto* ent = readdir(dir))
    {
        const std::string leaf = ent->d_name;
        if (leaf.size() < 11 || leaf.substr(leaf.size() - 10) != ".component") continue;
        const std::string full = dirPath + "/" + leaf;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            nullptr, (const UInt8*) full.c_str(), (CFIndex) full.size(), true);
        if (url == nullptr) continue;
        CFDictionaryRef info = CFBundleCopyInfoDictionaryInDirectory(url);
        CFRelease(url);
        if (info == nullptr) continue;

        auto* comps = (CFArrayRef) CFDictionaryGetValue(info, CFSTR("AudioComponents"));
        if (comps != nullptr && CFGetTypeID(comps) == CFArrayGetTypeID())
        {
            for (CFIndex i = 0; i < CFArrayGetCount(comps); ++i)
            {
                auto* c = (CFDictionaryRef) CFArrayGetValueAtIndex(comps, i);
                if (c == nullptr || CFGetTypeID(c) != CFDictionaryGetTypeID()) continue;
                auto str = [] (CFDictionaryRef d, CFStringRef k) -> std::string
                {
                    auto* v = (CFStringRef) CFDictionaryGetValue(d, k);
                    return (v != nullptr && CFGetTypeID(v) == CFStringGetTypeID())
                               ? cfStringToStd(v) : std::string();
                };
                const std::string t = str(c, CFSTR("type"));
                const std::string s = str(c, CFSTR("subtype"));
                const std::string m = str(c, CFSTR("manufacturer"));
                if (t.size() != 4 || s.size() != 4 || m.size() != 4) continue;
                auto* vn = (CFNumberRef) CFDictionaryGetValue(c, CFSTR("version"));
                if (vn == nullptr || CFGetTypeID(vn) != CFNumberGetTypeID()) continue;
                long long v = 0;
                if (! CFNumberGetValue(vn, kCFNumberLongLongType, &v)) continue;
                char dotted[40];
                std::snprintf(dotted, sizeof(dotted), "%lld.%lld.%lld",
                              (v >> 16) & 0xffff, (v >> 8) & 0xff, v & 0xff);
                out[t + "," + s + "," + m] = dotted;
            }
        }
        CFRelease(info);
    }
    closedir(dir);
}

std::vector<AUEntry> enumerateAUs()
{
    std::vector<AUEntry> results;

    // Once per scan: triple -> real version, from the same folders the AU
    // registry serves component bundles out of.
    std::unordered_map<std::string, std::string> plistVersions;
    harvestPlistVersions("/Library/Audio/Plug-Ins/Components", plistVersions);
    if (const char* home = std::getenv("HOME"))
        harvestPlistVersions(std::string(home) + "/Library/Audio/Plug-Ins/Components",
                             plistVersions);

    const OSType auTypes[] = {
        kAudioUnitType_Effect,
        kAudioUnitType_MusicEffect,
        kAudioUnitType_MusicDevice,
        kAudioUnitType_Generator,
    };

    for (auto auType : auTypes)
    {
        AudioComponentDescription searchDesc;
        searchDesc.componentType         = auType;
        searchDesc.componentSubType      = 0;
        searchDesc.componentManufacturer = 0;
        searchDesc.componentFlags        = 0;
        searchDesc.componentFlagsMask    = 0;

        AudioComponent comp = nullptr;
        while ((comp = AudioComponentFindNext(comp, &searchDesc)) != nullptr)
        {
            AudioComponentDescription actual;
            if (AudioComponentGetDescription(comp, &actual) != noErr) continue;

            CFStringRef cfName = nullptr;
            if (AudioComponentCopyName(comp, &cfName) != noErr || cfName == nullptr) continue;

            std::string fullName = cfStringToStd(cfName);
            CFRelease(cfName);

            std::string manufacturer, name;
            auto colon = fullName.find(':');
            if (colon != std::string::npos)
            {
                manufacturer = trim(fullName.substr(0, colon));
                name         = trim(fullName.substr(colon + 1));
            }
            else
            {
                name = trim(fullName);
            }
            if (name.empty()) continue;

            AUEntry e;
            e.name         = name;
            e.manufacturer = manufacturer;
            e.identifier   = buildAUIdentifier(actual);
            e.isInstrument = (auType == kAudioUnitType_MusicDevice);
            e.category     = e.isInstrument ? "Instrument" : "Effect";
            // Keyed by this component's OWN triple, so there is nothing to
            // misclassify: the value being replaced is the one just built
            // from that same triple. Miss keeps the triple (uid path).
            const std::string triple = osTypeToStd(actual.componentType)   + ","
                                     + osTypeToStd(actual.componentSubType) + ","
                                     + osTypeToStd(actual.componentManufacturer);
            const auto pv = plistVersions.find(triple);
            e.version      = (pv != plistVersions.end()) ? pv->second : triple;
            e.uniqueId     = (int)actual.componentType
                           ^ (int)actual.componentSubType
                           ^ (int)actual.componentManufacturer;
            results.push_back(std::move(e));
        }
    }

    return results;
}

#else // non-Apple

std::vector<AUEntry> enumerateAUs()
{
    return {};
}

#endif
