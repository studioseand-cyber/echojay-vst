/*
  EchoJayAuRegistry.h

  The ONE AU enumeration surface. Shared by ejextract and ejmap.

  Why this exists
  ---------------
  This tree grew three separate AU walks: ejextract's --au-registry, ejmap's
  PluginScanner, and Source/AUEnumerator.mm. They used four different component
  type filters and three copies of the identifier builder. Enumeration drift is
  how the Waves shells went invisible, so there is now one walk and everything
  else calls it.

  What is canonical, measured rather than assumed
  -----------------------------------------------
  The walk is JUCE's. AudioUnitPluginFormat::searchPathsForPlugins IS the
  AudioComponentFindNext loop (juce_AudioUnitPluginFormatHeadless.mm), including
  the component type filter and the AUv3 exclusion. Re-deriving that loop by
  hand is what produced four filters. So buildCensus() delegates the walk and
  owns only the policy on top: vendor parsing and the Apple/EchoJay exclusions.

  Baseline on the development machine, 2026-07-29, for the behaviour diff:
      1419 components enumerated
      1376 candidates after excluding Apple (40) and EchoJay (3)

  NOTHING HERE INSTANTIATES A PLUGIN.
  ------------------------------------
  In JUCE 8.0.12, AudioUnitPluginFormat::findAllTypesForFile calls
  createInstanceFromDescription: resolving one identifier loads the plugin. Doing
  that across the registry is ~1355 in-process loads from a single click, and it
  additionally returns EMPTY, silently, when called on the message thread for any
  plugin that needs an unblocked one.

  describeFromRegistry() exists so no caller ever needs it. Every field it fills
  comes from AudioComponentCopyName / AudioComponentGetVersion / the component
  description itself. Measured: name, version and uid for all 1419 components in
  0.06 seconds, zero instantiation.

  The fields it CANNOT know are left at their defaults, never guessed:
  numInputChannels and numOutputChannels require a live instance. ejmap does not
  display them; anything that needs them must load the plugin.

  tests/RegistryConformanceTest.cpp instantiates a bounded sample and asserts
  this header agrees with JUCE's own fillInPluginDescription field by field. If
  JUCE changes how it builds a description, that test fails rather than ejmap
  quietly mapping against a description EchoJay would not recognise.

  Collapsing Source/AUEnumerator.mm into this header is deliberately left open.
  It is a fourth copy of the same walk and belongs here, but it sits inside the
  ChainHost merge and is a separate decision. The surface below is shaped so it
  can move in without a redesign: it needs the census plus describeFromRegistry,
  and nothing else.

  House style: no em-dashes.

  Requires JUCE modules: juce_audio_processors, juce_core.
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <map>
#include <vector>

#if JUCE_MAC
 #include <AudioToolbox/AudioToolbox.h>
#endif

namespace echojay
{
namespace auregistry
{

//==============================================================================
/** One registered AU component, identified but NOT resolved.

    identifier is JUCE's own form ("AudioUnit:Effects/aufx,SILM,ksWV"), produced
    by AudioUnitFormatHelpers::createPluginIdentifier inside searchPathsForPlugins.
    It is what createPluginInstance expects, and it is unique per component.
*/
struct AuTarget
{
    juce::String identifier;
    juce::String typeCode;      // 4-char component type: "aufx", "aumf", "aumu", ...
    juce::String vendorCode;    // 4-char OSType, empty when the identifier did not parse
    juce::String vendorName;    // display name, or the raw code when unmapped
};

/** Result of one walk. Every number here is a count of something the walk SAW,
    so a caller can report what it dropped instead of reporting only what it
    kept. A filter that discards without counting is the silent-drop class.
*/
struct AuCensus
{
    int enumerated     = 0;     // components JUCE's walk returned
    int excludedApple  = 0;     // componentManufacturer 'appl'
    int excludedEcho   = 0;     // componentManufacturer 'Ecjy', our own plugins
    int unparsed       = 0;     // identifier yielded no 4-char manufacturer (KEPT)

    std::vector<AuTarget>       targets;    // what survived the exclusions
    std::map<juce::String, int> byVendor;   // vendor display name -> count
};

//==============================================================================
/** Manufacturer OSType from a JUCE AU identifier: the last comma-separated
    field of the segment after the final '/'. Empty when it does not parse,
    which is a kept-and-counted case, not a dropped one.
*/
inline juce::String manufacturerCodeOf (const juce::String& identifier)
{
    auto tail = identifier.fromLastOccurrenceOf ("/", false, false);   // "aufx,SILM,ksWV"
    auto code = tail.fromLastOccurrenceOf (",", false, false);
    return code.length() == 4 ? code : juce::String();
}

/** Component type OSType from a JUCE AU identifier: the FIRST comma-separated
    field of the segment after the final '/'. Empty when it does not parse.

    Callers use this to report what they dropped by type. Deciding which types
    are interesting is a caller's policy; the census keeps JUCE's set intact so
    the decision is visible rather than baked into the walk.
*/
inline juce::String componentTypeOf (const juce::String& identifier)
{
    auto tail = identifier.fromLastOccurrenceOf ("/", false, false);
    auto code = tail.upToFirstOccurrenceOf (",", false, false);
    return code.length() == 4 ? code : juce::String();
}

/** Manufacturer code to display name, from the AU registry survey. Unknown
    codes display as the raw code. Only 'appl' and 'Ecjy' are ever EXCLUDED, and
    that happens in buildCensus, not here.
*/
inline juce::String vendorDisplayName (const juce::String& code)
{
    static const std::map<juce::String, juce::String> m {
        { "ksWV", "Waves" }, { "appl", "Apple" }, { "SSLN", "SSL" },
        { "McDP", "McDSP" }, { "Ecjy", "EchoJay" }, { "Brwx", "Plugin Alliance" },
        { "-NI-", "Native Instruments" }, { "SfTb", "Softube" },
        { "HRSN", "Harrison" }, { "VST ", "Antares" }, { "OekS", "oeksound" },
    };
    auto it = m.find (code);
    return it != m.end() ? it->second : code;
}

//==============================================================================
/** Walks the AU component registry. Instantiates nothing.

    The walk and its component type filter are JUCE's, deliberately. allowAsync
    is false, which excludes AUv3: those require an unblocked message thread and
    cannot be resolved from a synchronous scan.
*/
inline AuCensus buildCensus()
{
    AuCensus census;

#if JUCE_MAC && JUCE_PLUGINHOST_AU
    juce::AudioUnitPluginFormat au;

    // Enumeration only. This returns identifier strings and runs no plugin code.
    auto ids = au.searchPathsForPlugins (au.getDefaultLocationsToSearch(),
                                         /*recursive*/ true,
                                         /*allowAsync (AUv3)*/ false);

    census.enumerated = ids.size();

    for (const auto& id : ids)
    {
        const auto code = manufacturerCodeOf (id);

        if (code == "appl") { ++census.excludedApple; continue; }   // AUGraphicEQ et al
        if (code == "Ecjy") { ++census.excludedEcho;  continue; }
        if (code.isEmpty()) ++census.unparsed;                      // kept: resolve later

        const auto vendor = vendorDisplayName (code.isEmpty() ? juce::String ("(unparsed)") : code);
        census.byVendor[vendor]++;
        census.targets.push_back ({ id, componentTypeOf (id), code, vendor });
    }
#endif

    return census;
}

//==============================================================================
#if JUCE_MAC && JUCE_PLUGINHOST_AU
namespace detail
{
    /** Reverse of JUCE's osTypeToString, for turning an identifier back into a
        component description we can look up. Kept private: callers deal in
        identifiers, never in OSTypes.
    */
    /** MEASURE WHAT YOU READ. This guarded on the TRIMMED length and then read
        four UNTRIMMED bytes, so every space-padded OSType -- and AU subtypes
        shorter than four characters are space-padded, "CR  ", "PM  ", "EB  " --
        failed the guard and returned 0.

        0 is not an error value to AudioComponentFindNext. It is a WILDCARD, so
        the caller silently received the first component matching type and
        manufacturer instead of the one it asked for: all 17 space-padded
        Soundtoys aufx components resolved to Crystallizer and all 3 aumf ones
        to EffectRack, sharing one uid, one identity key and four fingerprint
        collisions between them. Measured 4 Aug 2026; Little Plate ("LPL8", a
        full four characters) was the only Soundtoys AU that survived, which is
        the control case.

        Exactly four BYTES, because exactly four bytes are read. A longer token
        is a mis-parse and a multi-byte character would make the indexing below
        read something other than what it counted.
    */
    inline OSType stringToOSType (const juce::String& s)
    {
        if (s.getNumBytesAsUTF8() != 4)
            return 0;

        auto utf8 = s.toUTF8();
        return (((OSType) (unsigned char) utf8[0]) << 24)
             | (((OSType) (unsigned char) utf8[1]) << 16)
             | (((OSType) (unsigned char) utf8[2]) << 8)
             |  ((OSType) (unsigned char) utf8[3]);
    }

    /** "AudioUnit:Effects/aufx,SILM,ksWV" -> the three OSTypes.

        Splits exactly as JUCE's getComponentDescFromIdentifier does, on the
        LATER of the final ':' and the final '/'. Using '/' alone silently
        mis-parses the no-path-segment form ("AudioUnit:aufx,SILM,ksWV"), which
        JUCE emits for any component type outside its seven, taking "Audi" as
        the type code.
    */
    inline bool descriptionFromIdentifier (const juce::String& identifier,
                                           AudioComponentDescription& out)
    {
        auto tail = identifier.substring (juce::jmax (identifier.lastIndexOfChar (':'),
                                                      identifier.lastIndexOfChar ('/')) + 1);

        juce::StringArray tokens;
        tokens.addTokens (tail, ",", juce::StringRef());
        tokens.removeEmptyStrings();

        if (tokens.size() != 3)
            return false;

        juce::zerostruct (out);
        out.componentType         = stringToOSType (tokens[0]);
        out.componentSubType      = stringToOSType (tokens[1]);
        out.componentManufacturer = stringToOSType (tokens[2]);

        // A ZERO IS A WILDCARD, NEVER A QUERY. AudioComponentFindNext treats a
        // zero field as "match anything", so passing a failed parse through
        // turns "which component is this identifier" into "give me any
        // component" -- and it answers, confidently, with the wrong product.
        // A component in the registry cannot legitimately carry a zero code in
        // any of the three fields, so this refuses rather than asks.
        if (out.componentType == 0 || out.componentSubType == 0
             || out.componentManufacturer == 0)
        {
            juce::zerostruct (out);
            return false;
        }
        return true;
    }
}
#endif

/** Builds the PluginDescription for one registered component WITHOUT loading it.

    Mirrors JUCE's AudioUnitPluginInstance::fillInPluginDescription for every
    field obtainable from the registry. The two are compared field by field, on
    real plugins, by tests/RegistryConformanceTest.cpp.

    numInputChannels and numOutputChannels are left at 0: they are properties of
    a prepared instance, not of the registry, and inventing them would be the
    same class of lie as inventing a ui_hint coordinate.

    Returns a description with an empty fileOrIdentifier if the identifier does
    not resolve to a registered component. Callers must check.
*/
inline juce::PluginDescription describeFromRegistry (const juce::String& identifier)
{
    juce::PluginDescription desc;

#if JUCE_MAC && JUCE_PLUGINHOST_AU
    AudioComponentDescription cd {};
    if (! detail::descriptionFromIdentifier (identifier, cd))
        return desc;

    AudioComponent comp = AudioComponentFindNext (nullptr, &cd);
    if (comp == nullptr)
        return desc;

    // Take the registry's own description back: componentFlags and any field
    // the identifier does not carry come from here, not from our parse.
    AudioComponentDescription actual {};
    if (AudioComponentGetDescription (comp, &actual) != noErr)
        return desc;

    // A MEASUREMENT TAKEN AND NOT COMPARED IS NOT A CHECK. `actual` was
    // already being fetched here and then discarded, so when the lookup above
    // matched a DIFFERENT component -- which it did for every space-padded
    // subtype, via the zero-wildcard defect fixed in stringToOSType -- this
    // function had the evidence in hand and returned the wrong product's name,
    // vendor and uid anyway. The identifier is the question; the found
    // component must be the answer to THAT question or there is no answer.
    //
    // This is the second, independent guard: the parse could be wrong again in
    // some future form, and a caller must never receive a description for a
    // component it did not name.
    if (actual.componentType         != cd.componentType
         || actual.componentSubType      != cd.componentSubType
         || actual.componentManufacturer != cd.componentManufacturer)
        return desc;

    // AudioComponentCopyName reads registry metadata. It does not open the
    // component. JUCE splits the same string on ": " into manufacturer / name.
    juce::String fullName;
    {
        CFStringRef cfName = nullptr;
        if (AudioComponentCopyName (comp, &cfName) == noErr && cfName != nullptr)
        {
            fullName = juce::String::fromCFString (cfName);
            CFRelease (cfName);
        }
    }

    juce::String name = fullName, manufacturer;
    if (fullName.containsChar (':'))
    {
        manufacturer = fullName.upToFirstOccurrenceOf (":", false, false).trim();
        name         = fullName.fromFirstOccurrenceOf (":", false, false).trim();
    }

    // JUCE's fallback, matched exactly. Without it a nameless component reads
    // as an empty row here and as "<Unknown>" once loaded.
    if (name.isEmpty())
        name = "<Unknown>";

    juce::String version;
    {
        UInt32 versionNum = 0;
        if (AudioComponentGetVersion (comp, &versionNum) == noErr)
            version << (int)  (versionNum >> 16) << "."
                    << (int) ((versionNum >> 8) & 0xff) << "."
                    << (int)  (versionNum & 0xff);
    }

    desc.name             = name;
    desc.descriptiveName  = name;
    desc.fileOrIdentifier = identifier;
    desc.pluginFormatName = "AudioUnit";
    desc.manufacturerName = manufacturer;
    desc.version          = version;

    // JUCE's formula, from the component description alone. NOTE: this XOR is
    // NOT unique. Measured on a 1419-component registry: 2 collisions across 4
    // Waves components (Sibilance-Live vs EMO-Generator). It stays here as
    // metadata because EchoJay's maps carry it, but nothing may key on it.
    // See ejmap's ScannedPlugin::pluginId().
    desc.uniqueId = desc.deprecatedUid = ((int) actual.componentType)
                                       ^ ((int) actual.componentSubType)
                                       ^ ((int) actual.componentManufacturer);

    desc.isInstrument      = (actual.componentType == kAudioUnitType_MusicDevice);
    desc.lastInfoUpdateTime = juce::Time::getCurrentTime();

    // numInputChannels / numOutputChannels deliberately left at 0. See above.
#else
    juce::ignoreUnused (identifier);
#endif

    return desc;
}

} // namespace auregistry
} // namespace echojay
