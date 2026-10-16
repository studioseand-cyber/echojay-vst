/*
  EjmapSelfGuard.h - THE GUARD ON CLAUDE'S OWN LOADS (Kathy, 16 Oct 2026: "before ANY plugin load you run yourself on my Mac - tests, sound
  checks, proofs - filter the list through a guard that drops anything licence-bound. If a proof needs a licence-bound plugin, ask me first.")

  It guards the developer's own commands (ejmap --wizard-soundcheck, ejmap --self-guard for scripts), never the wizard a user runs: the
  wizard decides licences by their window, once, and remembers them. Dropped, in this order, each with its reason:
    - UAD (EjmapUadPreflight.h's rule), McDSP APB (EjmapApbPreflight.h's rule)
    - Soundtoys, Kilohearts (and any "kHs " product), SSL (vendor SSL / Solid State Logic, or an "SSL " product)
    - anything that has shown a licence window on this Mac: tools/ejmap/scripts/self_guard_windows.json (seeded 16 Oct, 210 products) and the
      wizard's own licence_windows.json in each EchoJay Wizard folder (read only), by plugin id or product name
    - PACE / iLok, by the bundle (EjmapCertDriver.h's paceHeld - a bundle that cannot be checked is held too). MeldaProduction included:
      its optional iLok route still makes it PACE-wrapped, and the rule is "drop anything PACE"
  The decision (judge) is pure and pinned (WZ-GUARD-1); the only way past it is --allow-licence-bound, used only after Kathy says yes.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapUadPreflight.h"
#include "EjmapApbPreflight.h"
#include <set>

namespace ejmap::selfguard
{

struct Memory { std::set<juce::String> products, ids; };   // lower case
struct Verdict { bool drop = false; juce::String why; };

inline Verdict judge (const juce::String& name, const juce::String& vendor, const juce::String& pluginId, bool paceHeld, const juce::String& paceWhy, const Memory& mem)
{
    const auto v = vendor.trim();
    if (uad::isUadProduct (name, v)) return { true, "UAD" };
    if (apb::isApbProduct (name, v)) return { true, "McDSP APB" };
    if (v.containsIgnoreCase ("Soundtoys")) return { true, "Soundtoys" };
    if (v.containsIgnoreCase ("Kilohearts") || name.trim().startsWith ("kHs ")) return { true, "Kilohearts" };
    if (v.equalsIgnoreCase ("SSL") || v.containsIgnoreCase ("Solid State Logic") || name.trim().startsWith ("SSL ")) return { true, "SSL" };
    if (mem.ids.count (pluginId.toLowerCase()) || mem.products.count (name.trim().toLowerCase())) return { true, "showed a licence window on this Mac before" };
    if (paceHeld) return { true, "PACE / iLok: " + paceWhy };
    return {};
}

inline void addMemoryFrom (Memory& m, const juce::var& v)
{
    if (const auto* a = v.getProperty ("products", {}).getArray()) for (const auto& x : *a) m.products.insert (x.toString().trim().toLowerCase());
    if (const auto* a = v.getProperty ("plugin_ids", {}).getArray()) for (const auto& x : *a) m.ids.insert (x.toString().trim().toLowerCase());
    if (const auto* a = v.getArray()) for (const auto& x : *a) m.ids.insert (x.toString().trim().toLowerCase());   // a wizard licence_windows.json
}
// the seed file in the repo and every wizard folder's own memory (read only)
inline Memory loadMemory (const juce::File& seed)
{
    Memory m; addMemoryFrom (m, juce::JSON::parse (seed.loadFileAsString()));
    const auto support = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
    for (const auto& app : support.findChildFiles (juce::File::findDirectories, false, "EchoJay Wizard*"))
        addMemoryFrom (m, juce::JSON::parse (app.getChildFile ("engine").getChildFile ("licence_windows.json").loadFileAsString()));
    return m;
}

} // namespace ejmap::selfguard
