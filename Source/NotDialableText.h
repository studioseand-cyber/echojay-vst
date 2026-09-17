#pragma once
// NotDialableText (hurdle 1 item 3, 17 Sep 2026): the words for a slot that is
// NOT DIALABLE under dial-only, and the built-in alternative named by the
// slot's role/category (pitch -> EchoJay Pitch, ...). One composer for the
// summary line's vocabulary and the reply card's sentence.
#include <JuceHeader.h>

inline juce::String builtinAlternativeForRole (const juce::String& roleOrCategory)
{
    const auto r = roleOrCategory.trim().toLowerCase();
    if (r.isEmpty()) return {};
    if (r.contains ("pitch") || r.contains ("tune"))            return "EchoJay Pitch";
    if (r.contains ("de-ess") || r.contains ("deess") || r.contains ("de ess")) return "EchoJay De-Esser";
    if (r.contains ("limit"))                                   return "EchoJay Limiter";
    if (r.contains ("expand"))                                  return "EchoJay Expander";
    if (r.contains ("gate"))                                    return "EchoJay Gate";
    if (r.contains ("comp") || r.contains ("dynamic"))          return "EchoJay Compressor";
    if (r.contains ("verb"))                                    return "EchoJay Reverb";
    if (r.contains ("delay") || r.contains ("echo"))            return "EchoJay Delay";
    if (r.contains ("sat") || r.contains ("tape") || r.contains ("drive") || r.contains ("harmonic")) return "EchoJay Saturation";
    if (r.contains ("eq") || r.contains ("equal") || r.contains ("filter")) return "EchoJay EQ";
    return {};
}

inline juce::String notDialableSentence (const juce::String& pluginName, const juce::String& reason,
                                         const juce::String& builtinAlternative)
{
    juce::String s;
    s << pluginName << " is NOT DIALABLE (" << (reason.isEmpty() ? juce::String ("no map for fp, no near map") : reason) << ")";
    if (builtinAlternative.isNotEmpty()) s << " - swap it for " << builtinAlternative << ", which EchoJay dials exactly,";
    else                                 s << " -";
    s << " or set its values by hand from its card.";
    return s;
}
