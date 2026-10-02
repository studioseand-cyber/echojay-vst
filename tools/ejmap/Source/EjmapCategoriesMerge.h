/*
  EjmapCategoriesMerge.h - categories.json, merged from the server's reply WITH THE KEYS DISCOVERY READS.

  Found by the unbroken stranger's-Mac test (2 Oct, 13:16): the categorise endpoint's reply names a product
  ("name|vendor") and its category, but carries no mark_keys ("AudioUnit|<uid>") and no members, and
  certification's discovery indexes categories.json BY mark_keys - so a fresh Mac's categories.json
  categorised 0 identities and the batch found nothing to do. The 26 Aug file the seeded rehearsal used had
  come from a different, local tool that wrote the keys. The keys are derivable here, from the scan rows
  the request was built from, so the merge stamps them: on a served entry, and on an existing entry that
  lacks them. An existing entry that has them is left alone (the mapper's own categorisation is not rewritten).
*/
#pragma once
#include <juce_core/juce_core.h>
#include <map>

namespace ejmap::categoriesmerge
{
// What the scan knows about one product key: the mark keys ("Format|uidhex") and members (format identifiers).
struct ProductKeys { juce::StringArray markKeys, members; };

inline int mergeServed (juce::var& doc, const juce::var& served, const std::map<juce::String, ProductKeys>& keysByProduct)
{
    auto* obj = served.getDynamicObject();
    auto* docObj = doc.getDynamicObject();
    if (docObj == nullptr) { docObj = new juce::DynamicObject(); doc = juce::var (docObj); }
    auto prods = doc.getProperty ("products", juce::var());
    auto* prodObj = prods.getDynamicObject();
    if (prodObj == nullptr) { prodObj = new juce::DynamicObject(); docObj->setProperty ("products", juce::var (prodObj)); }
    auto stamp = [&] (const juce::String& key, juce::var entry) {
        auto* e = entry.getDynamicObject(); if (e == nullptr) return entry;
        auto it = keysByProduct.find (key); if (it == keysByProduct.end()) return entry;
        if (! e->hasProperty ("mark_keys") || e->getProperty ("mark_keys").size() == 0) { juce::Array<juce::var> mk; for (const auto& k : it->second.markKeys) mk.add (k); e->setProperty ("mark_keys", mk); }
        if (! e->hasProperty ("members") || e->getProperty ("members").size() == 0)     { juce::Array<juce::var> mm; for (const auto& m : it->second.members) mm.add (m); e->setProperty ("members", mm); }
        return entry; };
    int n = 0;
    if (obj != nullptr)
        for (const auto& kv : obj->getProperties())
            if (! prodObj->hasProperty (kv.name)) { prodObj->setProperty (kv.name, kv.value); ++n; }
    // ONE PASS stamps every entry without keys: the ones just served and the ones a reply alone wrote earlier
    for (const auto& kv : prodObj->getProperties()) prodObj->setProperty (kv.name, stamp (kv.name.toString(), kv.value));
    if (! docObj->hasProperty ("run")) { auto* run = new juce::DynamicObject(); run->setProperty ("tool", "categorise-endpoint/1"); docObj->setProperty ("run", juce::var (run)); }
    return n;
}
} // namespace ejmap::categoriesmerge
