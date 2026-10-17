/*
  EjmapSelfGuardRun.h - the guard's list step (Kathy, 16 Oct): a developer command's plugin names resolved against the installed AUs,
  each judged by EjmapSelfGuard.h before anything loads it. One header on both branches (feat/ejmap-cert and feat/ejmap-wizard).
*/

#pragma once

#include "EjmapCertDriver.h"
#include "EjmapSelfGuard.h"

namespace ejmap::cert
{

// THE GUARD (Kathy, 16 Oct, EjmapSelfGuard.h): every developer command that loads plugins resolves its names here - licence-bound ones are
// dropped and said, unless allowLicenceBound (--allow-licence-bound, only after Kathy says yes). Nothing is loaded to decide
// allowNames (17 Oct, Sean's pack): products let through by name - the guard still judges and says each one; everything else stays guarded
inline std::vector<juce::PluginDescription> guardedPlugins (const juce::StringArray& names, bool allowLicenceBound, const juce::String& tag, juce::StringArray* dropped = nullptr,
                                                            const juce::StringArray* allowNames = nullptr)
{
    const auto mem = selfguard::loadMemory (juce::File (EJMAP_REPO_ROOT).getChildFile ("tools/ejmap/scripts/self_guard_windows.json"));
    const auto bundles = componentBundles();
    std::vector<juce::PluginDescription> ds; juce::StringArray found;
    for (const auto& r : installedAudioUnits())
    {
        if (! names.contains (r.desc.name) || found.contains (r.desc.name)) continue;
        found.add (r.desc.name);
        juce::String pw; const bool pace = paceHeld (r.desc, bundles, pw);
        const auto v = selfguard::judge (r.desc.name, r.desc.manufacturerName, "AudioUnit|" + juce::String::toHexString (r.desc.uniqueId).toLowerCase() + "|" + r.desc.version, pace, pw, mem);
        const bool allowed = allowLicenceBound || (allowNames != nullptr && allowNames->contains (r.desc.name));
        if (v.drop && ! allowed) { std::cout << tag << ": GUARD dropped " << r.desc.name << " (" << v.why << ")" << std::endl; if (dropped) dropped->add (r.desc.name + "\t" + v.why); continue; }
        if (v.drop) std::cout << tag << ": GUARD let through (" << (allowLicenceBound ? "--allow-licence-bound" : "the allow list") << "): " << r.desc.name << " (" << v.why << ")" << std::endl;
        ds.push_back (r.desc);
    }
    for (const auto& n : names) if (! found.contains (n)) std::cout << tag << ": not installed: " << n << std::endl;
    return ds;
}
} // namespace ejmap::cert
