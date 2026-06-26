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
    std::string version;       // osType triplet, e.g. "aufx,Comp,AAPL"
    bool        isInstrument { false };
    int         uniqueId     { 0 };
};

// Enumerate AU plugins from the CoreAudio registry without instantiating them.
// Implemented in AUEnumerator.mm (no JUCE headers) to avoid the AudioBuffer conflict.
// Returns an empty vector on non-Apple platforms.
std::vector<AUEntry> enumerateAUs();
