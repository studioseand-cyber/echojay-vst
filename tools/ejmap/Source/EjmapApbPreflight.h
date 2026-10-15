/*
  EjmapApbPreflight.h - IS A McDSP APB UNIT PRESENT? (Kathy, 15 Oct 2026: the four APB compressors on her Mac, with no APB unit,
  measured digital silence for 3-8 minutes each in the serial lane - C-18 / C673-A "silent_output", MC-3 / Royal Mu "needs_licence".)

  An APB plugin is McDSP's DSP-hosted line: the AU's manufacturer is McDSP and its name starts "APB " (the bundles are named
  "*_APB.component"). Without the APB-8 / APB-16 hardware it loads, shows its controls, and passes no audio, so it cannot be
  measured - the same case as a UAD plugin without a UAD-2 device (EjmapUadPreflight.h), and handled the same way: looked for ONCE
  per process by the unit's names in the I/O registry and the hardware listing (USB, Thunderbolt, PCI); absent -> the wizard's scan
  files every APB plugin "needs_hardware", not loaded, not measured, checked again on the next scan.

  McDSP's own software (APB Control / its helper processes and .app bundles) says the SOFTWARE is installed, never that a unit is
  there, so a line naming it does not count. A unit reached only over a network may not appear in the registry: --assume-apb-device
  takes it as present for that run, and the matching lines are kept so the result can be judged. Not seen with a unit here (Kathy's
  Mac has none); the decision rule is PURE (deviceFromLines) and pinned (WZ-HW-1).
*/

#pragma once

#include <juce_core/juce_core.h>

namespace ejmap::apb
{

inline bool isApbProduct (const juce::String& pluginName, const juce::String& manufacturer)
{
    return pluginName.trim().startsWithIgnoreCase ("APB ") && manufacturer.containsIgnoreCase ("McDSP");
}

// the lines that name a unit, out of a registry / profiler dump; McDSP's software (an app, a helper, a bundle path) does not count
inline juce::StringArray deviceLines (const juce::String& dump)
{
    juce::StringArray out;
    for (const auto& line : juce::StringArray::fromLines (dump))
    {
        const auto l = line.toLowerCase();
        if (! (l.contains ("apb-8") || l.contains ("apb-16") || l.contains ("apb8") || l.contains ("apb16") || (l.contains ("mcdsp") && l.contains ("apb")))) continue;
        if (l.contains (".app") || l.contains (".component") || l.contains ("/applications/") || l.contains ("/library/") || l.contains ("apb control")) continue;
        out.add (line.trim());
    }
    return out;
}
struct Device { bool present = false; juce::StringArray lines; juce::String how; };
inline Device deviceFromLines (const juce::String& ioregDump, const juce::String& profilerDump)
{
    Device d; d.lines.addArray (deviceLines (ioregDump)); d.lines.addArray (deviceLines (profilerDump));
    d.present = ! d.lines.isEmpty();
    d.how = d.present ? "an APB unit named in the I/O registry / hardware listing (" + juce::String (d.lines.size()) + " line(s))"
                      : juce::String ("no McDSP APB unit in the I/O registry or the hardware listing");
    return d;
}
inline juce::String runCapture (const juce::StringArray& args)
{
    juce::ChildProcess p; if (! p.start (args)) return {};
    const auto out = p.readAllProcessOutput(); p.waitForProcessToFinish (10000); return out;
}
inline const Device& device (bool assumePresent = false)
{
    static Device d; static bool done = false;
    if (! done)
    {
        done = true;
        d = deviceFromLines (runCapture ({ "/usr/sbin/ioreg", "-r", "-l", "-w0" }),
                             runCapture ({ "/usr/sbin/system_profiler", "SPThunderboltDataType", "SPUSBDataType", "SPPCIDataType", "-detailLevel", "mini" }));
        for (auto& l : d.lines) if (l.length() > 200) l = l.substring (0, 200) + "...";
    }
    if (assumePresent && ! d.present) { static Device forced; forced = d; forced.present = true; forced.how = "--assume-apb-device: taken as present; the registry showed none"; return forced; }
    return d;
}
inline constexpr const char* kNotConnected = "APB unit not connected";
inline bool& assumeFlag() { static bool f = false; return f; }   // --assume-apb-device (every command)

} // namespace ejmap::apb
