#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <set>
#include <vector>
#include "EchoJayAuRegistry.h"

/*
  WHICH .component BUNDLES THE REGISTRY PASS ALREADY COVERS (29 Aug 2026).

  THE PROBLEM THIS SOLVES. --bootstrap walks the filesystem, and a shell bundle
  answers a path with ONE component: the 13 WaveShell bundles yielded 13
  fingerprints against ~600 Waves plugins. --au-registry enumerates components
  individually and reached all of them (measured 27 Jul: Waves 614/614 ok, 614
  fingerprints, p50 1.62s). Running both walks over AU would instantiate every
  non-shell AU twice, and the two ledgers cannot be reconciled to prevent it:
  bootstrap_ledger keys on BUNDLE PATH with sig "version|newest-mtime", while
  au_registry_ledger keys on MD5(IDENTIFIER) with sig = the identifier string.
  Different key spaces, different sig semantics, no cheap mapping between them --
  AuTarget deliberately carries no bundle path, because resolving an identifier
  means instantiating the plugin.

  SO THE WALKS ARE PARTITIONED, NOT RECONCILED. The file walk keeps VST3; the
  registry takes AU. A .component yields only AudioUnit types and a .vst3 only
  VST3 types, so the two cover disjoint format spaces and no dedupe is needed.

  THE ONE THING THAT MUST BE CHECKED, not assumed: that the registry really is a
  superset, or dropping the AU file walk loses plugins silently. This header is
  that check, and it runs EVERY time rather than trusting a number measured once.

  IDENTITY WITHOUT INSTANTIATION. A .component's Info.plist declares its
  AudioComponents (type/subtype/manufacturer), so its identifiers can be built
  and compared against the census without loading any plugin code. Measured on
  this machine: 751 of 766 bundles expose them; 748 of 763 non-EchoJay bundles
  are fully covered by the census, and ZERO have an identifier the census lacks.

  FAILS SAFE, AND THE FAILURE IS THE CARVE-OUT. A bundle is left to the file walk
  when its Info.plist cannot be read, when it declares no AudioComponents, or
  when ANY of its identifiers is absent from the census. "I could not prove the
  registry covers this" is treated as "walk it", never as "skip it". The 15
  bundles that hit this today are all unreadable-Info.plist, not missing
  identifiers -- but the rule is written for the case that has not happened yet.
*/
namespace echojay::aucoverage
{

/** JUCE's identifier prefix for a component type code. Mirrors the forms seen in
    the live census: aufx/aumf -> Effects, aumi -> MidiEffects, aumu -> Synths. */
inline juce::String prefixForType (const juce::String& type)
{
    if (type == "aumu") return "Synths";
    if (type == "aumi") return "MidiEffects";
    if (type == "augn") return "Generators";
    if (type == "aupn") return "Panners";
    return "Effects";                        // aufx, aumf, and anything unknown
}

/** "AudioUnit:Effects/aufx,SILM,ksWV" from the three OSType strings. */
inline juce::String identifierFor (const juce::String& type,
                                   const juce::String& subtype,
                                   const juce::String& manufacturer)
{
    return "AudioUnit:" + prefixForType (type) + "/" + type + "," + subtype + "," + manufacturer;
}

/** Every AU identifier a .component DECLARES, read from Info.plist. Empty when
    the plist is missing, unparseable, or declares no AudioComponents -- all of
    which the caller must treat as "cannot prove coverage". */
inline juce::StringArray declaredIdentifiers (const juce::File& bundle)
{
    juce::StringArray out;
    auto plist = bundle.getChildFile ("Contents/Info.plist");
    if (! plist.existsAsFile()) return out;
    auto xml = juce::XmlDocument::parse (plist);            // handles XML plists
    if (xml == nullptr) return out;
    // <dict><key>AudioComponents</key><array><dict>...type/subtype/manufacturer
    std::function<juce::XmlElement*(juce::XmlElement*)> findComponents =
        [&] (juce::XmlElement* dict) -> juce::XmlElement*
    {
        if (dict == nullptr) return nullptr;
        for (auto* c = dict->getFirstChildElement(); c != nullptr; c = c->getNextElement())
            if (c->hasTagName ("key") && c->getAllSubText().trim() == "AudioComponents")
                return c->getNextElement();
        return nullptr;
    };
    auto* root = xml->getChildByName ("dict");
    auto* arr  = findComponents (root);
    if (arr == nullptr || ! arr->hasTagName ("array")) return out;
    for (auto* d = arr->getFirstChildElement(); d != nullptr; d = d->getNextElement())
    {
        if (! d->hasTagName ("dict")) continue;
        juce::String type, subtype, manu, pendingKey;
        for (auto* c = d->getFirstChildElement(); c != nullptr; c = c->getNextElement())
        {
            if (c->hasTagName ("key")) { pendingKey = c->getAllSubText().trim(); continue; }
            const auto v = c->getAllSubText();
            if      (pendingKey == "type")         type    = v;
            else if (pendingKey == "subtype")      subtype = v;
            else if (pendingKey == "manufacturer") manu    = v;
            pendingKey.clear();
        }
        if (type.isNotEmpty() && subtype.isNotEmpty() && manu.isNotEmpty())
            out.addIfNotAlreadyThere (identifierFor (type, subtype, manu));
    }
    return out;
}

/** Why a bundle stayed in the file walk. Reported, never silent. */
struct CarveReason
{
    juce::File   bundle;
    juce::String why;      // "no-identifiers" | "not-in-census:<id>"
};

struct Coverage
{
    std::set<juce::String> censusIds;
    int  componentsSeen    = 0;
    int  componentsCovered = 0;
    std::vector<CarveReason> carved;

    /** True when the registry pass will reach every component this bundle
        declares, so the file walk may skip it. */
    bool covers (const juce::File& bundle) const
    {
        for (const auto& c : carved) if (c.bundle == bundle) return false;
        return true;
    }
};

/** Decide, for every .component bundle in `bundles`, whether the census covers
    it. Pure apart from reading Info.plists; instantiates nothing. */
inline Coverage assess (const juce::Array<juce::File>& bundles,
                        const echojay::auregistry::AuCensus& census)
{
    Coverage cov;
    for (const auto& t : census.targets) cov.censusIds.insert (t.identifier);
    for (const auto& b : bundles)
    {
        if (b.getFileExtension().toLowerCase() != ".component") continue;
        if (b.getFileName().startsWithIgnoreCase ("EchoJay")) continue;
        ++cov.componentsSeen;
        const auto ids = declaredIdentifiers (b);
        if (ids.isEmpty()) { cov.carved.push_back ({ b, "no-identifiers" }); continue; }
        juce::String missing;
        for (const auto& id : ids)
            if (cov.censusIds.find (id) == cov.censusIds.end()) { missing = id; break; }
        if (missing.isNotEmpty()) cov.carved.push_back ({ b, "not-in-census:" + missing });
        else ++cov.componentsCovered;
    }
    return cov;
}

} // namespace echojay::aucoverage
