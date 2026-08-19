/*
  EchoJayParamMaps.h

  Fingerprint + display helpers for EchoJay auto-parameter-mapping.

  The fingerprint MUST match EchoJayParamExtractor.h makeFingerprint exactly
  (format|uidHex|version|param_count, SHA-256 hex): the server's seeded maps
  (plugin:{fp}:map, GET /api/params/maps?fps=...) are keyed by it. param_count
  requires a LOADED instance, so fingerprints are computed at slot-load time,
  and ChainHost persists identity(format|uid|version) -> fp so scan-time
  prefetch can batch-fetch maps for every plugin seen at least once.

  House style: no em-dashes.

  Requires JUCE modules: juce_audio_processors, juce_core, juce_cryptography.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <map>

namespace echojay
{

// Keep byte-identical to the extractor: order matters and must be stable.
inline juce::String fingerprintForDescription (const juce::PluginDescription& desc, int paramCount)
{
    juce::String basis;
    basis << desc.pluginFormatName << "|"
          << juce::String::toHexString (desc.uniqueId) << "|"
          << desc.version << "|"
          << juce::String (paramCount);

    juce::SHA256 sha (basis.toRawUTF8(), basis.getNumBytesAsUTF8());
    return sha.toHexString();
}

// Identity WITHOUT param_count: what a PluginDescription alone can provide.
// Used as the persistent index key identity -> fp once a load has revealed
// the parameter count.
inline juce::String identityKeyForDescription (const juce::PluginDescription& desc)
{
    return desc.pluginFormatName + "|" + juce::String::toHexString (desc.uniqueId) + "|" + desc.version;
}

// One plugin as the /api/params/lookup body wants it: ik is
// identityKeyForDescription (format|uidHex|version; the server ignores the
// version segment in exists mode), and manufacturer is a SEPARATE field, not
// part of ik. The dialable answer is matched back by ik.
struct IdentityRef
{
    juce::String ik;
    juce::String manufacturer;
};

// How fpForIdentity resolved (or refused) a lookup. One bucket per call,
// so a caller counting these reconciles against its input count exactly.
enum class FpLookup { exact, uidFallback, ambiguous, miss, noUid };

// identity -> fp with a version-insensitive fallback. Exact identity key
// first; if that misses, format|uid alone, but only when every version key
// for that pair agrees on ONE fp. An ambiguous uid (two version keys, two
// fps) returns nothing rather than guessing: the version is the fp
// discriminator, and a wrong fp would serve another binary's controls.
//
// The ambiguity refusal is NOT airtight (12 Aug 2026). It only fires once
// BOTH binaries have been fingerprinted. The case it does not cover is a
// plugin updated but never loaded since: the index holds exactly one fp
// for that uid, it is stale, and this returns it confidently. Shipped
// anyway because the exact overlay has never applied on this machine (the
// entries cache's AU version field carries the component triple, not a
// version, so the exact key misses systematically), which means every
// exposure today is sibling-merged across AU and VST3 variants, and a
// possibly-one-version-stale exact fp is strictly more precise than that.
// Once completeLoad fingerprints the updated binary the uid holds two fps
// and the refusal takes over on its own.
inline juce::String fpForIdentity (const std::map<juce::String, juce::String>& identityToFp,
                                   const juce::PluginDescription& desc,
                                   FpLookup* outcome = nullptr)
{
    auto resolve = [outcome] (FpLookup o, const juce::String& fp)
    {
        if (outcome != nullptr) *outcome = o;
        return fp;
    };

    // uniqueId == 0 is NO IDENTITY, refused before any lookup (13 Aug
    // 2026). Every thin VST3 scan row carries uniqueId=0, so all of them
    // share the identity prefix VST3|0|: a COLLIDING key, not a missing
    // one. The two-fingerprint case would refuse as ambiguous and is safe;
    // the dangerous case is ONE fingerprint indexed under VST3|0|, which
    // the uid fallback would hand to every remaining zero-uid VST3 plugin
    // on the machine. Refused even for an exact-shaped key: a zero-uid
    // index entry is itself corrupt and must never serve.
    if (desc.uniqueId == 0)
        return resolve (FpLookup::noUid, {});

    auto it = identityToFp.find (identityKeyForDescription (desc));
    if (it != identityToFp.end())
        return resolve (FpLookup::exact, it->second);

    // Format names and hex uids never contain '|', so this prefix selects
    // exactly the version keys of this format|uid pair.
    const juce::String uidPrefix = desc.pluginFormatName + "|"
                                 + juce::String::toHexString (desc.uniqueId) + "|";
    juce::String found;
    for (const auto& kv : identityToFp)
    {
        if (! kv.first.startsWith (uidPrefix)) continue;
        if (found.isEmpty())                   { found = kv.second; continue; }
        if (found != kv.second)                return resolve (FpLookup::ambiguous, {});
    }
    if (found.isEmpty())
        return resolve (FpLookup::miss, {});
    return resolve (FpLookup::uidFallback, found);
}

// ---------------------------------------------------------------------------
// Stale-map ladder (12 Aug 2026). completeLoad is the ONLY point where index
// staleness is detectable: at buildMapFpsJson time nothing has been loaded,
// so a live fp differing from the indexed fp is the first and last proof the
// index was stale. The decisions are pure functions here so mapfps_test can
// pin the state table; ChainHost owns the side effects.
// ---------------------------------------------------------------------------

enum class StaleRung
{
    noDivergence,   // indexed fp equals live fp
    firstIndex,     // nothing indexed yet: new knowledge, not staleness
    refetch,        // stale index corrected, live-fp map fetch outstanding
    mapHeld,        // live-fp map present, dial verdict not in yet
    dialled,        // the apply RAN and WROTE (applied or partial)
    undialled,      // the apply ran against a present map and wrote nothing
    unmapped        // fetch answered and the corpus lacks this fp: card speaks
};

inline const char* staleRungName (StaleRung r)
{
    switch (r)
    {
        case StaleRung::noDivergence: return "no-divergence";
        case StaleRung::firstIndex:   return "first-index";
        case StaleRung::refetch:      return "refetch-pending";
        case StaleRung::mapHeld:      return "map-held";
        case StaleRung::dialled:      return "dialled";
        case StaleRung::undialled:    return "undialled";
        case StaleRung::unmapped:     return "refetched-unmapped";
    }
    return "?";
}

struct StaleLadderStep
{
    StaleRung rung;
    bool correctIndex;    // write the live fp into the index
    bool kickRefetch;     // request the live fp's map now
    bool markSlot;        // remember the divergence on the slot until resolved
};

// At load. Divergence corrects the index, marks the slot and (when the live
// fp's map is absent) kicks the refetch. It does NOT gate value safety any
// more (12 Aug 2026, the whole-set refusal lived here for a few hours):
// real divergence is the same uid at a different version, where control
// names and ranges are usually identical, so refusing everything discarded
// dialling that would have been correct - the rehearsal that motivated the
// refusal pointed one plugin's identity at a COMPLETELY DIFFERENT plugin,
// which cannot occur naturally, and the response was tuned to it. Wrong
// values are now caught at the value level by valueWithinMappedRange
// (EchoJayParamApply.h), on every turn, diverged or not. Divergence keeps
// driving the card wording (the unmapped note, and the intent reframe when
// out-of-range refusals coincide with a diverged slot) and the refetch.
inline StaleLadderStep staleLadderAtLoad (const juce::String& indexedFp,
                                          const juce::String& liveFp,
                                          bool mapHeldForLiveFp)
{
    if (indexedFp.isNotEmpty() && indexedFp == liveFp)
        return { StaleRung::noDivergence, false, false, false };
    if (indexedFp.isEmpty())
        return { StaleRung::firstIndex, true, false, false };
    if (mapHeldForLiveFp)
        return { StaleRung::mapHeld, true, false, true };
    return { StaleRung::refetch, true, true, true };
}

// At resolution. The dial result outranks the map bookkeeping: once the
// apply has run, dialled/undialled derive from whether anything WROTE,
// never from map presence. Before the apply, a present map defers the
// verdict (mapHeld), an in-flight fetch holds (refetch), and only an
// answered fetch with no map is unmapped.
inline StaleRung staleLadderAtResolution (bool answered, bool mapHeldForLiveFp,
                                          bool applyRan, bool wroteAnything)
{
    if (applyRan)         return wroteAnything ? StaleRung::dialled : StaleRung::undialled;
    if (mapHeldForLiveFp) return StaleRung::mapHeld;
    if (! answered)       return StaleRung::refetch;
    return StaleRung::unmapped;
}

// Compact human display of one applied semantic setting:
//   ratio "4:1" -> "ratio 4:1", attack_ms 40 -> "attack 40ms",
//   threshold_db -18 -> "threshold -18dB", freq_hz 1200 -> "freq 1200Hz",
//   mix_pct 25 -> "mix 25%", reverb_decay_s 2 -> "reverb decay 2s".
inline juce::String formatSemanticSetting (const juce::String& key, const juce::var& value)
{
    // Display rounding (9 Aug 2026): values that crossed a float32 render
    // as "0.050000000745058" - the card's sibling of the prompt-range fix.
    // Display only; the applied value is untouched.
    const juce::String v = value.isDouble()
        ? juce::String ((double) value, 4).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
        : value.toString();
    if (key == "ratio")       return "ratio " + v;
    if (key.endsWith ("_db"))  return key.dropLastCharacters (3).replaceCharacter ('_', ' ') + " " + v + "dB";
    if (key.endsWith ("_ms"))  return key.dropLastCharacters (3).replaceCharacter ('_', ' ') + " " + v + "ms";
    if (key.endsWith ("_hz"))  return key.dropLastCharacters (3).replaceCharacter ('_', ' ') + " " + v + "Hz";
    if (key.endsWith ("_pct")) return key.dropLastCharacters (4).replaceCharacter ('_', ' ') + " " + v + "%";
    if (key.endsWith ("_s"))   return key.dropLastCharacters (2).replaceCharacter ('_', ' ') + " " + v + "s";
    return key + " " + v;
}

} // namespace echojay
