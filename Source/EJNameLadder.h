#pragma once
#include <JuceHeader.h>
#include "ChainHost.h"            // stripParenthetical: ONE base-name rule
#include "EJVariantPreference.h"  // the channel-variant rank, unchanged and reused

// THE NAME->REGISTRATION MATCH LADDER, and the channel variant it now prefers.
//
// WHY THIS MOVED OUT OF ChainHost.cpp (28 Aug 2026). resolveByName is the ONLY
// resolver most paths have: the chain-edit add/replace ops, saved-chain recall
// with a bare name, every Link path (a Link process never calls
// buildRecommendable, so its feed table is permanently empty and
// resolveOfferedName degenerates to this ladder), and the build path's own
// fallback. 9c4f629 taught the FEED to prefer the stereo registration and this
// ladder was left alone, so the two disagreed: a build turn loaded CLA-76 (s)
// and an add op naming the same plugin loaded CLA-76 (m), measured live. The
// audio consequence is not cosmetic -- a mono build refuses the rack's 2x2
// layout, buildGraph passes the right channel round it undirected, and a stereo
// source comes out with its two sides diverging.
//
// It is a header because mapfps_test links the PREVIOUS build's SharedCode lib:
// a fix left inside ChainHost.cpp could not be run by a pin without a rebuild,
// and this commit carries none. Compiled here, the gate runs the SHIPPED ladder
// over the real registry AND compares it, name by name, against the lib's old
// one -- which is what makes "byte-identical for everything that is not a
// channel variant" a measurement rather than an argument.
//
// THE RUNGS, AND WHICH ONE RANKS.
//   exact                       UNTOUCHED, and deliberately first. A name that
//                               carries a suffix means that registration:
//                               "CLA-76 (m)" must still give the mono build,
//                               which is what a saved chain, a Link sidecar and
//                               the picker all hand it.
//   stripped (+manufacturer)    RANKS. The manufacturer tie-break still wins
//                               first, because a caller who wrote
//                               "Name (Vendor)" asked for a vendor, not a
//                               channel count.
//   normalised                  RANKS. This is the rung every collapsed base
//                               name lands on, and the one that was returning
//                               whichever registration sorted first.
// RANKING IS NOT FILTERING, and the distinction is the whole safety argument: a
// product whose only registration is mono still resolves, to that mono build,
// because the rank only chooses between rows that already matched. Equal ranks
// keep the first, so a name with one candidate resolves exactly as it did:
// measured against the OLD implementation over every name in the registry, 227
// of 1,724 move, and all 227 are a WaveShell variant of the same product.
//
// THE FORMAT FILTER COMPOSES BY CONSTRUCTION. The pool is built by the caller,
// after the format filter and the withhold gate; this function only chooses
// within it. A VST3-filtered pool with no Waves rows in it ranks nothing.
namespace echojay
{

// Normalize a plugin name for fuzzy matching:
//   lowercase, trim whitespace, collapse internal runs of spaces/punctuation
//   to a single space, strip trailing version suffixes like " 3" / " v2" / " 2.0".
inline juce::String normalizeName(const juce::String& raw)
{
    juce::String s = raw.toLowerCase().trim();
    // Replace common punctuation chars that differ between sources with space
    s = s.replace("-", " ").replace("_", " ").replace(".", " ");
    // Collapse multiple spaces
    while (s.contains("  ")) s = s.replace("  ", " ");
    // Strip trailing version tokens: " 3", " v2", " 2", " ii", " iii"
    s = s.trimEnd();
    juce::StringArray parts = juce::StringArray::fromTokens(s, " ", "");
    if (parts.size() >= 2)
    {
        const auto& last = parts[parts.size() - 1];
        bool isVersion = last.containsOnly("0123456789") ||
                         (last.startsWithChar('v') && last.substring(1).containsOnly("0123456789")) ||
                         last == "ii" || last == "iii" || last == "iv";
        if (isVersion) parts.remove(parts.size() - 1);
    }
    return parts.joinIntoString(" ").trim();
}

// The trailing all-digits token normalizeName would strip, or empty. Mirrors
// that tokenization so the number it returns is exactly the one stripped. Only
// bare digits count as a model (v2 / II / III stay version suffixes); those are
// still stripped and never guarded.
inline juce::String trailingModelNumber(const juce::String& raw)
{
    juce::String s = raw.toLowerCase().trim();
    s = s.replace("-", " ").replace("_", " ").replace(".", " ");
    while (s.contains("  ")) s = s.replace("  ", " ");
    juce::StringArray parts = juce::StringArray::fromTokens(s.trim(), " ", "");
    if (parts.size() >= 2)
    {
        const auto& last = parts[parts.size() - 1];
        if (last.containsOnly("0123456789")) return last;
    }
    return {};
}

/** Of several registrations one rung accepted, which one the caller gets.

    Lower channel-variant rank wins (EJVariantPreference.h: no suffix, then (s),
    then (m), then (m->s), then anything else); a tie keeps the earliest hit, so
    the pool's own order still decides between equals and a single candidate is
    returned unchanged. `hits` must not be empty. */
inline int preferredOfHits (const juce::Array<juce::PluginDescription>& pool,
                            const juce::Array<int>& hits)
{
    int win = hits[0];
    for (int i : hits)
        if (channelVariantIsBetter (pool.getReference (i).name,
                                    pool.getReference (win).name))
            win = i;
    return win;
}

/** The match ladder: exact / stripped / stripped+manufacturer / normalised.

    ONE function so the offered pool and the withheld pool are searched by
    identical rules: a name that would have resolved to a withheld row is
    reported as withheld, never as not found. Returns the index into pool, or
    -1; `how` receives the rung that matched.

    `raw` is the trimmed request, `base` its parenthetical-stripped form, `manu`
    the text of that parenthetical when there was one (a manufacturer hint). */
inline int matchInPool (const juce::Array<juce::PluginDescription>& pool,
                        const juce::String& raw,
                        const juce::String& base,
                        const juce::String& manu,
                        juce::String& how)
{
    for (int i = 0; i < pool.size(); ++i)
        if (pool.getReference(i).name.equalsIgnoreCase(raw)) { how = "exact"; return i; }

    // Parenthetical-stripped match, manufacturer as tie-breaker
    juce::Array<int> baseHits;
    for (int i = 0; i < pool.size(); ++i)
        if (pool.getReference(i).name.equalsIgnoreCase(base)) baseHits.add(i);
    if (baseHits.size() == 1) { how = "stripped"; return baseHits[0]; }
    if (baseHits.size() > 1)
    {
        if (manu.isNotEmpty())
            for (int i : baseHits)
                if (pool.getReference(i).manufacturerName.containsIgnoreCase(manu))
                { how = "stripped+manufacturer"; return i; }
        const int win = preferredOfHits (pool, baseHits);
        how = (win == baseHits[0] ? "stripped (first of several)"
                                  : "stripped (preferred variant)");
        return win;
    }

    // Normalised (case/punctuation/version-token tolerant). Model-number guard:
    // normalizeName strips a trailing number as a version, which collapses
    // "AMEK EQ 250" and "AMEK EQ 200" to the same stem. When the request AND
    // the candidate each carry a trailing number and they DIFFER, the number is
    // a model, not a version, so it is not a match. A number on only one side
    // (e.g. "Saturn 2" vs a plugin named "Saturn") still tolerates the strip.
    const auto keyIn = normalizeName (base);
    const auto numIn = trailingModelNumber (base);
    juce::Array<int> normHits;
    for (int i = 0; i < pool.size(); ++i)
    {
        const auto& d = pool.getReference(i);
        if (normalizeName(ChainHost::stripParenthetical(d.name)) != keyIn) continue;
        const auto numCand = trailingModelNumber(ChainHost::stripParenthetical(d.name));
        if (numIn.isNotEmpty() && numCand.isNotEmpty() && numIn != numCand) continue;
        normHits.add (i);
    }
    if (! normHits.isEmpty())
    {
        const int win = preferredOfHits (pool, normHits);
        how = (win == normHits[0] ? "normalised" : "normalised (preferred variant)");
        return win;
    }
    return -1;
}

} // namespace echojay
