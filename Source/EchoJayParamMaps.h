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
// PRODUCT identity (17 Sep 2026 ruling): format + plugin uid - never the
// version, never the parameter count. A map belongs to the PRODUCT; the fp
// (format|uid|version|paramCount) stays a cache key and a log field only.
inline juce::String productKeyForDescription (const juce::PluginDescription& desc)
{
    return desc.pluginFormatName + "|" + juce::String::toHexString (desc.uniqueId);
}
inline juce::String productKeyOfIdentity (const juce::String& ik)   // "format|uidHex|version" -> "format|uidHex"
{
    const int a = ik.indexOfChar ('|'); if (a < 0) return ik;
    const int b = ik.indexOfChar (a + 1, '|'); return b < 0 ? ik : ik.substring (0, b);
}

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
// THE SYNCED IDENTITY STORE (21q item 1, 23 Sep 2026). See
// ~/echojay-saas/CONTRACT_SYNC_2026-09-23.md.
//
// A fingerprint is only learned locally by LOADING a plugin, so on a 1429-plugin
// machine mapFps carried 63 of them and the server was told nothing about the
// other 1366. POST /api/params/sync hands the server the inventory as identity
// keys and it answers with the fingerprint it already holds. Those answers live
// here, beside - never inside - identityToFp_: a probe-derived fingerprint was
// measured on THIS machine from the binary the user actually holds and always
// wins; a synced one is the server's best answer for that identity.
// ---------------------------------------------------------------------------
struct SyncedIdentity
{
    juce::String fp;        // format|uid|version|paramCount, as the server holds it
    juce::String version;   // the version the fp belongs to
    juce::String tier;      // OPAQUE to the client: stored and logged, never interpreted
};

// What POST /api/params/sync actually keys on, VERIFIED LIVE 23 Sep 2026 against the deployed endpoint: the server
// resolves by NAME plus manufacturer/format/version, not by identity key. Sending {ik, manufacturer} returns
// name:"" and tier:"none" for every row, and the endpoint's own error body names the shape it wants:
//     {"error":"bad_body","message":"expected { plugins: [ { name, manufacturer?, format?, version? } ] }"}
// ik is carried along so the ANSWER can be stored under the client's own identity key - the server never echoes it.
struct SyncRef
{
    juce::String ik;             // client-side storage key (format|uidHex|version); never sent
    juce::String uid;            // the hex uid out of the PRODUCT identity - what the server resolves on FIRST
    juce::String name;           // the fallback the server resolves on when the uid is unknown to it
    juce::String manufacturer;
    juce::String format;
    juce::String version;
};

inline SyncRef syncRefForDescription (const juce::PluginDescription& desc)
{
    // uid is the same hex the product key carries (format|uidHex), so the client cannot hand the server one
    // spelling of a uid here and another there.
    return { identityKeyForDescription (desc),
             productKeyForDescription (desc).fromLastOccurrenceOf ("|", false, false),
             desc.name, desc.manufacturerName, desc.pluginFormatName, desc.version };
}

// The request body, as a pure function so a guard pins the WIRE SHAPE instead of describing it. This shape has
// moved twice in two days (ik-keyed -> name-keyed -> uid-first, 24 Sep), which is exactly why it is pinned.
inline juce::String buildSyncRequestBody (const std::vector<SyncRef>& refs)
{
    juce::Array<juce::var> arr;
    for (const auto& r : refs)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("uid", r.uid);                  // resolved FIRST by the server
        o->setProperty ("name", r.name);                // fallback when the uid is unknown
        o->setProperty ("manufacturer", r.manufacturer);
        o->setProperty ("format", r.format);
        o->setProperty ("version", r.version);
        arr.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("plugins", arr);
    return juce::JSON::toString (juce::var (root), true);
}

// The response parse, as a pure function so a guard can pin it against a VERBATIM live body.
// Live shape (23 Sep 2026):
//   {"count":N,"mapped":M,"unmapped":U,"tiers":{...},
//    "results":[{"i":0,"name":"Pro-Q 3","mapped":true,"tier":"exact",
//                "fp":"e9ec8039...","version":"3.2.3","versions":["3.2.5","3.2.3"]}],"ms":372}
// Rows are joined back by "i", the INDEX INTO THE BATCH THAT WAS SENT - the server does not echo the identity
// key, so an out-of-range or missing index is unjoinable and is dropped rather than guessed at.
// A row with mapped:false or a null fp is not an answer and is not stored.
inline std::map<juce::String, SyncedIdentity> parseSyncResponse (const juce::var& json,
                                                                 const std::vector<SyncRef>& sent,
                                                                 int* unjoinableOut = nullptr)
{
    std::map<juce::String, SyncedIdentity> out;
    int unjoinable = 0;
    if (auto* results = json.getProperty ("results", juce::var()).getArray())
    {
        for (auto& row : *results)
        {
            const auto iv = row.getProperty ("i", juce::var());
            if (! iv.isInt() && ! iv.isDouble()) { ++unjoinable; continue; }
            const int i = (int) iv;
            if (i < 0 || i >= (int) sent.size())  { ++unjoinable; continue; }
            const auto fp = row.getProperty ("fp", juce::var());
            if (fp.isVoid() || ! fp.isString() || fp.toString().isEmpty()) continue;   // null fp: not an answer
            if (! (bool) row.getProperty ("mapped", juce::var (true)))               continue;
            const auto ver = row.getProperty ("version", juce::var()).toString();
            out[sent[(size_t) i].ik] = { fp.toString(),
                                         ver.isNotEmpty() ? ver : sent[(size_t) i].version,
                                         row.getProperty ("tier", juce::var()).toString() };
        }
    }
    if (unjoinableOut != nullptr) *unjoinableOut = unjoinable;
    return out;
}

// ---------------------------------------------------------------------------
// THE SYNC LATCH (24 Sep 2026 ruling). A scan generation is latched ONLY by an answer that actually carried
// something. Zero mapped rows, a non-200, or a transport error are NOT answers: the generation stays unlatched,
// the next launch asks again, and the log says which of the three it was.
//
// WHY THE RULE EXISTS. The first cut latched "on completion, ok or 404", which is right for the existence index
// (a 404 there means the feature is off) and WRONG here: the endpoint answered every row tier:"none" for a plugin
// it holds a map for, and a latch on that would have frozen a whole inventory out of mapFps until the user
// rescanned. An empty answer is indistinguishable, client-side, from a server that has nothing - so it must be
// retried, not believed.
//
// It is a plain object rather than two loose members so the guard can drive the SAME state machine the editor
// runs, instead of a re-description of it.
// ---------------------------------------------------------------------------
struct SyncLatch
{
    juce::String answeredSig;      // the identity signature an answer actually arrived for

    bool shouldAsk (const juce::String& sig) const { return sig.isNotEmpty() && sig != answeredSig; }

    // ok: a clean 200 whose body parsed. mappedRows: how many rows carried an fp. Returns the log reason.
    juce::String recordAnswer (bool ok, int mappedRows, const juce::String& askedSig)
    {
        if (! ok)
            return "no answer (non-200 or transport error) - NOT latched, the next launch asks again";
        if (mappedRows <= 0)
            return "answered with 0 mapped row(s) - NOT latched, the next launch asks again";
        answeredSig = askedSig;
        return "answered with " + juce::String (mappedRows) + " mapped row(s) - latched for this scan generation";
    }
};

// Exact identity only. No uid fallback here on purpose: the client asked about
// exactly these identity keys and the server answered about exactly these
// identity keys, so a near-miss is a different binary, not a near answer. The
// version-insensitive tolerance stays where it was earned, in fpForIdentity.
inline juce::String syncedFpForIdentity (const std::map<juce::String, SyncedIdentity>& synced,
                                          const juce::PluginDescription& desc)
{
    if (desc.uniqueId == 0) return {};                       // no identity, same refusal as fpForIdentity
    const auto it = synced.find (identityKeyForDescription (desc));
    return it == synced.end() ? juce::String() : it->second.fp;
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

    // A LABEL GETS A COLON; A NUMBER DOES NOT (25 Aug 2026).
    //
    // Pro-Q 3's mode control is NAMED "Band 1 Used" and its labels are
    // {Unused, Used}, so the fall-through rendered "Band 1 Used Used": the
    // name and the value separated by a space, with nothing to say which is
    // which. Not rare -- 442 control/label pairs across 37 of the 128 cached
    // maps have a name ending in one of its own labels ("HP On/Off"+"Off",
    // "EQ High Bell"+"Bell"), and six have a name EQUAL to a label ("In"+"In").
    //
    // Deleting the duplicate would be worse than the duplicate. "Band 1 Used"
    // alone is a bare control name, indistinguishable from the card merely
    // mentioning the control, and its two states would read "Band 1 Used" and
    // "Band 1 Used Unused". The separator carries the distinction the line
    // exists for; removing a word destroys it.
    //
    // NUMBERS ARE UNTOUCHED, and the guard is not decoration: semanticToFloat
    // accepts a STRING and parses a number out of it, so "7" can reach an
    // anchored control, apply, and stay a string in requestedValue. Keying the
    // colon on isString() alone would then turn "Vol 7" into "Vol: 7" -- a
    // numeric render changing, which is exactly what must not happen. So a
    // value that opens like a number keeps the space. Cost, measured: 4 of the
    // corpus's 6,881 mode labels are numeric text (SPL IRON's sign switches,
    // label "0") and keep today's spacing.
    //
    // The TIERED line is deliberately not converging on this. It renders modes
    // as `Band 1 Used -> reads "Used"` for the model; this is the card, for a
    // person, and the two are different lines for different readers.
    const bool opensLikeNumber = v.isEmpty()
                              || juce::CharacterFunctions::isDigit (v[0])
                              || v[0] == '-' || v[0] == '+' || v[0] == '.';
    return key + (value.isString() && ! opensLikeNumber ? ": " : " ") + v;
}

} // namespace echojay
