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

std::vector<AUEntry> enumerateAUs()
{
    std::vector<AUEntry> results;

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
            e.version      = osTypeToStd(actual.componentType)        + ","
                           + osTypeToStd(actual.componentSubType)     + ","
                           + osTypeToStd(actual.componentManufacturer);
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
