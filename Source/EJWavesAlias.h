#pragma once
#include <JuceHeader.h>

// Marketing name -> WaveShell registration name, for the AI feed.
//
// THE SEAM. PluginScanner injects 69 curated Waves names (expandWavesCatalog),
// because a filesystem walk cannot see inside WaveShell. Those are Waves'
// MARKETING names. ChainHost's real AU scan sees the SHELL's names, which are
// often shorter or punctuated differently: "Renaissance Vox" is registered
// "RVox (m)" / "RVox (s)", "PuigTec EQP-1A" is "PuigTec EQP1A". 35 of the 69
// resolved to nothing, so plugins the user has ticked and installed were
// invisible to the model, which then told them the plugin was not in their
// library while it sat racked.
//
// ADDITIVE BY CONSTRUCTION. This is consulted ONLY after the existing lookup
// (model key, then stem, then the after-colon retry) has already missed, and it
// returns a BASE NAME that is then fed back through that same lookup. So no row
// that resolves today can resolve differently, and the (m)/(s) preference in
// EJVariantPreference.h still decides the variant.
//
// VARIANT COUNTS ARE RESOLVED BY OPERATOR DECISION (28 Aug 2026, second pass).
// Six marketing names cover several registered products differing only in how
// much of the product you get: band count, tap count, voice count. These were
// refused as ambiguous until an operator chose the rule, which is FULLEST
// VARIANT -- offer the model the most capable build and let it use less. They
// are in the explicit table below, not derived, because "fullest" is a
// judgement about products and nothing in the names encodes it.
//
// STILL REFUSED, and for a different reason: Abbey Road TG Mastering Chain is
// not installed on this machine under any spelling. That is an absence, not an
// ambiguity, and no table entry can fix it.
namespace echojay
{

/** Lowercased, non-alphanumerics removed. "PuigTec EQP-1A" -> "puigteceqp1a". */
inline juce::String wavesAlnumKey (const juce::String& s)
{
    juce::String out;
    for (auto c : s)
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            out << juce::CharacterFunctions::toLowerCase (c);
    return out;
}

/** The hand-written aliases, and why each one cannot be derived.
    Kept deliberately short: every entry here is a name the shell shortened in a
    way no rule recovers, and each is one-to-one against the installed set. */
inline const std::vector<std::pair<const char*, const char*>>& wavesExplicitAliases()
{
    static const std::vector<std::pair<const char*, const char*>> t = {
        // "Reverb" contracts to "Verb"; the initialism rule below yields
        // "RReverb", which does not exist.
        { "Renaissance Reverb",            "RVerb"                 },
        // The shell drops the middle word.
        { "Aphex Vintage Aural Exciter",   "Aphex Vintage Exciter" },
        // "Stereo" is dropped. S1 MS Matrix / S1 Shuffler are other products.
        { "S1 Stereo Imager",              "S1 Imager"             },
        // The SSL family is spelled solid and drops the console letter or the
        // descriptor entirely; nothing mechanical maps E-Channel -> SSLChannel.
        { "SSL E-Channel",                 "SSLChannel"            },
        { "SSL G-Equalizer",               "SSLEQ"                 },
        { "SSL G-Master Buss Compressor",  "SSLComp"               },
        // "Master" is dropped. The EKramer* rows are the Eddie Kramer series,
        // a different product line, so this stays one-to-one.
        { "Kramer Master Tape",            "Kramer Tape"           },

        // FULLEST-VARIANT CHOICES. Each of these marketing names covers two or
        // three registrations that differ only in capacity, and the operator's
        // rule is to offer the fullest. The target on the right is the name the
        // shell actually registers, which is NOT always the obvious spelling:
        // "API-550B" carries the API- prefix, "Doubler4" has no space, and
        // "TransX Multi" has no hyphen even though the marketing name does.
        { "Renaissance Equalizer",         "REQ 6"                 },  // of REQ 2 / 4 / 6
        { "API 550",                       "API-550B"              },  // of API-550A / B
        { "SuperTap",                      "SuperTap 6-Taps"       },  // of 2-Taps / 6-Taps
        { "Doubler",                       "Doubler4"              },  // of Doubler2 / Doubler4
        { "Trans-X",                       "TransX Multi"          },  // of TransX Multi / Wide
        { "NLS Non-Linear Summer",         "NLS Channel"           },  // of NLS Buss / Channel
    };
    return t;
}

/** The registration base name `scannerName` should resolve to, or empty.
    `registryBaseNames` are parenthetical-stripped names from the real scan.
    Returns empty when nothing matches AND when more than one thing does. */
inline juce::String wavesAliasFor (const juce::String& scannerName,
                                   const juce::StringArray& registryBaseNames)
{
    const auto want = wavesAlnumKey (scannerName);
    if (want.isEmpty()) return {};

    // 1. Explicit table first: a hand-written answer outranks any derivation.
    for (const auto& kv : wavesExplicitAliases())
        if (wavesAlnumKey (juce::String (kv.first)) == want)
        {
            const juce::String target (kv.second);
            for (const auto& b : registryBaseNames)
                if (wavesAlnumKey (b) == wavesAlnumKey (target)) return b;
            return {};   // aliased to something this machine does not have
        }

    // 2. Punctuation and spacing only: "PuigTec EQP-1A" -> "PuigTec EQP1A".
    {
        juce::StringArray hits;
        for (const auto& b : registryBaseNames)
            if (wavesAlnumKey (b) == want) hits.addIfNotAlreadyThere (b);
        if (hits.size() == 1) return hits[0];
        if (hits.size() > 1)  return {};
    }

    // 3. "Renaissance X" -> "RX". Derived, not tabled, because it is the
    //    pattern the family is named on. It resolves Vox and Compressor;
    //    Reverb needs the table above and Equalizer is genuinely ambiguous.
    {
        auto t = juce::StringArray::fromTokens (scannerName, " -_", "");
        t.removeEmptyStrings();
        if (t.size() >= 2 && t[0].equalsIgnoreCase ("Renaissance"))
        {
            juce::String c; c << t[0][0];
            for (int i = 1; i < t.size(); ++i) c << t[i];
            const auto key = wavesAlnumKey (c);
            juce::StringArray hits;
            for (const auto& b : registryBaseNames)
                if (wavesAlnumKey (b) == key) hits.addIfNotAlreadyThere (b);
            if (hits.size() == 1) return hits[0];
            if (hits.size() > 1)  return {};
        }
    }

    // 4. The shell name is a leading run of the marketing name:
    //    "H-Comp Hybrid Compressor" -> "H-Comp", "C4 Multiband Compressor" -> "C4".
    //    LONGEST match wins, which is what separates "Q10 Equalizer" -> "Q10"
    //    from the "Q1" that also prefixes it. A tie between two DIFFERENT names
    //    at the same length is an ambiguity and refuses.
    {
        juce::StringArray best; int bestLen = 0;
        for (const auto& b : registryBaseNames)
        {
            const auto k = wavesAlnumKey (b);
            if (k.length() < 2 || k == want) continue;
            if (! want.startsWith (k)) continue;
            if (k.length() > bestLen) { bestLen = k.length(); best.clear(); best.add (b); }
            else if (k.length() == bestLen) best.addIfNotAlreadyThere (b);
        }
        if (best.size() == 1) return best[0];
    }
    return {};
}

} // namespace echojay
