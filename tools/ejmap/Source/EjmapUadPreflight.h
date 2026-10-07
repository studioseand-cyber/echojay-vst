/*
  EjmapUadPreflight.h - IS A UAD-2 DEVICE PRESENT? (Sean, 6 Oct 2026: his UAD is a UAD-2 SATELLITE, not an Apollo; it runs at
  the host's rate, so a UAD plugin's window means "no device" (the Satellite not connected) or authorisation.)

  Before any UAD product loads, the device is looked for ONCE per process in the I/O registry (`ioreg -r -l -w0`: a
  connected Satellite - Thunderbolt, USB or FireWire - carries "UAD-2" or "Universal Audio" in its entries) and in the
  hardware listing (`system_profiler SPThunderboltDataType SPUSBDataType SPFireWireDataType SPPCIDataType`). The matching
  lines are kept so the result can be judged from the zip. Absent: every UAD product is filed "UAD-2 device not
  connected", no load, no window, once, said at the start of the run. The UAD Meter & Control Panel process running says
  the SOFTWARE is installed, not that a device is there (it runs here with none), so it is recorded but never counts.

  Nothing here is measured on a Mac with the device (none here): the detection is by the device's names in the registry.
  If Sean's Mac with the Satellite connected still reads "absent", `--assume-uad-device` overrides the gate for that run
  and the recorded lines say what the registry showed. The decision rule is PURE (deviceFromLines), pinned (UD1-UD3).
*/

#pragma once

#include <juce_core/juce_core.h>

namespace ejmap::uad
{

inline bool isUadProduct (const juce::String& pluginName, const juce::String& manufacturer) { return pluginName.startsWithIgnoreCase ("UAD ") || manufacturer.equalsIgnoreCase ("Universal Audio"); }

// the lines that name the device, out of a registry / profiler dump; a line about the control-panel SOFTWARE does not count
inline juce::StringArray deviceLines (const juce::String& dump)
{
    juce::StringArray out;
    for (const auto& line : juce::StringArray::fromLines (dump))
    {
        const auto l = line.toLowerCase();
        if (! (l.contains ("uad-2") || l.contains ("uad2") || l.contains ("universal audio"))) continue;
        if (l.contains ("meter & control panel") || l.contains ("uad mixer engine") || l.contains (".app") || l.contains ("/applications/")) continue;
        out.add (line.trim());
    }
    return out;
}
struct Device { bool present = false; juce::StringArray lines; bool panelRunning = false; juce::String how; };
inline Device deviceFromLines (const juce::String& ioregDump, const juce::String& profilerDump, bool panelRunning)
{
    Device d; d.panelRunning = panelRunning;
    d.lines.addArray (deviceLines (ioregDump)); d.lines.addArray (deviceLines (profilerDump));
    d.present = ! d.lines.isEmpty();
    d.how = d.present ? "UAD-2 device named in the I/O registry / hardware listing (" + juce::String (d.lines.size()) + " line(s))"
                      : juce::String ("no UAD-2 / Universal Audio device in the I/O registry or the hardware listing") + (panelRunning ? " (the UAD Meter & Control Panel is running: the software is installed, no device)" : juce::String());
    return d;
}

// the live check, once per process
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
        const auto ioreg = runCapture ({ "/usr/sbin/ioreg", "-r", "-l", "-w0" });
        const auto prof = runCapture ({ "/usr/sbin/system_profiler", "SPThunderboltDataType", "SPUSBDataType", "SPFireWireDataType", "SPPCIDataType", "-detailLevel", "mini" });
        const auto ps = runCapture ({ "/bin/ps", "-axo", "command" });
        d = deviceFromLines (ioreg, prof, ps.containsIgnoreCase ("UAD Meter & Control Panel") || ps.containsIgnoreCase ("UAD Mixer Engine"));
        for (auto& l : d.lines) if (l.length() > 200) l = l.substring (0, 200) + "...";
        if (d.lines.size() > 40) { d.lines.removeRange (40, d.lines.size() - 40); d.lines.add ("..."); }
    }
    if (assumePresent && ! d.present) { static Device forced; forced = d; forced.present = true; forced.how = "--assume-uad-device: taken as present; the registry showed none"; return forced; }
    return d;
}
inline juce::var deviceVar (const Device& d) { auto* o = new juce::DynamicObject(); o->setProperty ("present", d.present); o->setProperty ("how", d.how); o->setProperty ("panel_running", d.panelRunning); juce::Array<juce::var> ls; for (const auto& l : d.lines) ls.add (l); o->setProperty ("lines", ls); return juce::var (o); }

inline constexpr const char* kNotConnected = "UAD-2 device not connected";

} // namespace ejmap::uad
