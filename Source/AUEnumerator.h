#pragma once
#include <vector>
#include <string>

// Plain C++ struct — no JUCE or CoreAudio types exposed here.
// Keeping these headers separate prevents the include-order conflict between
// CoreAudio's "typedef struct AudioBuffer AudioBuffer" and juce::AudioBuffer<T>
// that arises when "using namespace juce;" (from JuceHeader.h) is in scope.
struct AUEntry
{
    std::string name;
    std::string manufacturer;
    std::string identifier;    // e.g. "AudioUnit:Effects/aufx,Comp,AAPL"
    std::string category;      // "Effect" | "Instrument" | "Generator"
    std::string version;       // real version ("12.0.0") when the component
                               // bundle's Info.plist declares one for this
                               // triple; the osType triplet otherwise (the
                               // pre-12-Aug-2026 behaviour, kept as the
                               // fallthrough so the uid path still works)
    bool        isInstrument { false };
    int         uniqueId     { 0 };
};

// Enumerate AU plugins from the CoreAudio registry without instantiating them.
// Implemented in AUEnumerator.mm (no JUCE headers) to avoid the AudioBuffer conflict.
// Returns an empty vector on non-Apple platforms.
std::vector<AUEntry> enumerateAUs();
