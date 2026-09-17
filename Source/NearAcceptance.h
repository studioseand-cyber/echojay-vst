#pragma once
// NearAcceptance (amendment 1, 17 Sep 2026): the PARTIAL acceptance rule for a
// same-name map from another build. A candidate is ACCEPTED when every mapped
// Tier-1 semantic (the params the model emits: key_root, scale, reference_hz,
// retune, ...) and every category ESSENTIAL (pitch: Key, Scale, Retune Speed,
// Detune/Reference; the other categories mirror lib/controls-note.js
// CATEGORY_ESSENTIALS) resolves on the live instance; unresolved NON-essential
// names are dropped from the working copy and listed. A candidate missing an
// essential or a mapped Tier-1 param is REJECTED with the names.
#include <JuceHeader.h>
#include <regex>
#include <vector>

namespace echojay
{
struct NearEssential { juce::String label, pattern; };
inline const std::vector<NearEssential>& nearEssentialsFor (const juce::String& category)
{
    static const std::vector<NearEssential> pitch      { { "Key", "^key$" }, { "Scale", "^scale$" }, { "Retune Speed", "retune\\s*speed" }, { "Detune/Reference", "^detune$|reference|^ref$" } };
    static const std::vector<NearEssential> compressor { { "Threshold", "thresh|\\bthr\\b" }, { "Ratio", "^ratio$" }, { "Attack", "attack|\\batk\\b" }, { "Release", "release|\\brel\\b|\\brcv\\b" } };
    static const std::vector<NearEssential> limiter    { { "Threshold/Ceiling", "thresh|\\bthr\\b|ceiling|\\bceil\\b" }, { "Release", "release|\\brel\\b" } };
    static const std::vector<NearEssential> eq         { { "Freq", "\\bfreq" }, { "Gain", "\\bgain\\b" }, { "Q", "\\bq\\b" } };
    static const std::vector<NearEssential> deesser    { { "Threshold", "thresh" }, { "Freq", "\\bfreq" } };
    static const std::vector<NearEssential> reverb     { { "Mix", "\\bmix\\b|dry\\s*/\\s*wet|\\bwet\\b" }, { "Decay", "decay|time|\\brt\\b" }, { "Pre-delay", "pre-?delay" } };
    static const std::vector<NearEssential> delay      { { "Mix", "\\bmix\\b|dry\\s*/\\s*wet|\\bwet\\b" }, { "Time", "time|delay" }, { "Feedback", "feedback" } };
    static const std::vector<NearEssential> saturation { { "Drive", "drive|saturat" }, { "Mix", "\\bmix\\b|dry\\s*/\\s*wet|\\bwet\\b" } };
    static const std::vector<NearEssential> none;
    const auto c = category.trim().toLowerCase();
    if (c == "pitch") return pitch;
    if (c == "compressor" || c == "dynamics") return compressor;
    if (c == "limiter") return limiter;
    if (c == "eq") return eq;
    if (c == "de-esser" || c == "deesser") return deesser;
    if (c == "reverb") return reverb;
    if (c == "delay") return delay;
    if (c == "saturation") return saturation;
    return none;
}

// True when `name` is one of the category's essentials (case-insensitive regex, as the server classifies).
inline bool nearMatches (const juce::String& pattern, const juce::String& name)
{
    return std::regex_search (name.trim().toLowerCase().toStdString(), std::regex (pattern.toStdString(), std::regex::icase));
}
inline bool nearIsEssential (const juce::String& category, const juce::String& name)
{
    for (const auto& e : nearEssentialsFor (category)) if (nearMatches (e.pattern, name)) return true;
    return false;
}
// The essentials the candidate does NOT carry as a RESOLVED control ("missing an essential"): each
// category essential must exist in the candidate AND resolve on the live instance.
inline juce::StringArray nearMissingEssentials (const juce::String& category, const juce::StringArray& resolvedControls)
{
    juce::StringArray missing;
    for (const auto& e : nearEssentialsFor (category))
    {
        bool found = false;
        for (const auto& n : resolvedControls) if (nearMatches (e.pattern, n)) { found = true; break; }
        if (! found) missing.add (e.label);
    }
    return missing;
}
} // namespace echojay
