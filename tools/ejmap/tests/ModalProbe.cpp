/*
  ModalProbe.cpp

  PROBE. Where does a vendor's "no hardware detected" alert actually fire, and
  WHOSE window is it?

  THE MEASUREMENT THAT PROMPTED IT. In a 20-plugin sweep, plugin 1 took 328
  seconds of which the load was 68 MILLISECONDS; the other 19 took 2-12 s each
  with identical 51-70 ms loads. So the alert did not fire in
  createPluginInstance and did not fire in editor creation -- the sweep had
  already stopped opening editors -- and it fired ONCE for the whole session,
  not once per plugin. That leaves the first parameter interaction.

  Loading without an editor therefore did NOT remove the class, and this decides
  what can: if the window belongs to THIS process it can be dismissed from a
  run-loop observer; if it belongs to a vendor helper it cannot, and the answer
  has to be to provoke it deliberately while an operator is present.

  Window ownership is read from CGWindowListCopyWindowInfo, which returns
  metadata -- owner name, pid, title, bounds -- and NOT pixels, so it needs no
  Screen Recording permission.

      ejmap-arch-probe --modal "AudioUnit:Effects/aufx,09au,!UAD"
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <unistd.h>
#include <iostream>
#include <set>

namespace
{
juce::String windowsNow (std::set<juce::String>& seen, pid_t self)
{
    juce::String out;
    CFArrayRef list = CGWindowListCopyWindowInfo (kCGWindowListOptionOnScreenOnly
                                                    | kCGWindowListExcludeDesktopElements,
                                                  kCGNullWindowID);
    if (list == nullptr) return out;

    for (CFIndex i = 0; i < CFArrayGetCount (list); ++i)
    {
        auto* d = (CFDictionaryRef) CFArrayGetValueAtIndex (list, i);
        auto str = [d] (CFStringRef k) -> juce::String
        {
            auto v = (CFStringRef) CFDictionaryGetValue (d, k);
            if (v == nullptr) return {};
            char buf[512] = {};
            CFStringGetCString (v, buf, sizeof buf, kCFStringEncodingUTF8);
            return juce::String::fromUTF8 (buf);
        };
        int pid = 0;
        if (auto* n = (CFNumberRef) CFDictionaryGetValue (d, kCGWindowOwnerPID))
            CFNumberGetValue (n, kCFNumberIntType, &pid);

        const auto owner = str (kCGWindowOwnerName);
        const auto title = str (kCGWindowName);
        // Only windows worth reporting: ours, or anything with a title.
        if (pid != (int) self && title.isEmpty())
            continue;

        const auto key = juce::String (pid) + "|" + owner + "|" + title;
        if (seen.insert (key).second)
            out << "    NEW WINDOW  pid " << pid << (pid == (int) self ? "  (THIS PROCESS)" : "")
                << "  owner " << owner.quoted() << "  title " << title.quoted() << "\n";
    }
    CFRelease (list);
    return out;
}
}

int runModalProbe (const juce::String& identifier, int seconds)
{
    const pid_t self = getpid();
    std::cout << "modal probe, pid " << self << "\n"
              << "subject   : " << identifier << std::endl;

    // Named rather than addDefaultFormats(): this probe hosts one format and
    // adding the rest drags in constructors this target does not compile.
    juce::AudioPluginFormatManager mgr;
    auto* au = new juce::AudioUnitPluginFormat();
    mgr.addFormat (au);

    juce::OwnedArray<juce::PluginDescription> found;
    au->findAllTypesForFile (found, identifier);
    if (found.isEmpty())
    { std::cerr << "no description for " << identifier << std::endl; return 2; }

    std::set<juce::String> seen;
    std::cout << "baseline windows:\n" << windowsNow (seen, self) << std::flush;

    juce::String err;
    const auto t0 = juce::Time::getMillisecondCounter();
    std::unique_ptr<juce::AudioPluginInstance> inst (
        mgr.createPluginInstance (*found[0], 48000.0, 512, err));
    const auto loadMs = juce::Time::getMillisecondCounter() - t0;

    if (inst == nullptr)
    { std::cerr << "instantiate failed: " << err << std::endl; return 2; }

    std::cout << "instantiated in " << loadMs << " ms, "
              << inst->getParameters().size() << " params, NO EDITOR CREATED\n"
              << windowsNow (seen, self)
              << "  -- if nothing new appeared above, the alert is NOT on instantiation\n"
              << std::flush;

    // AUDIO FIRST. The sweep runs a silent pump -- prepareToPlay then repeated
    // processBlock -- before it touches anything, and a "no hardware detected"
    // alert is far more likely to come from a DSP-accelerated plugin being
    // asked to render than from a parameter write. The probe follows the same
    // order the sweep does so a negative result means something.
    {
        std::cout << "\nprepareToPlay + 200 blocks ..." << std::endl;
        inst->prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buf (juce::jmax (2, inst->getTotalNumInputChannels()), 512);
        juce::MidiBuffer midi;
        for (int b = 0; b < 200; ++b)
        {
            buf.clear();
            inst->processBlock (buf, midi);
        }
        std::cout << windowsNow (seen, self) << std::flush;
    }

    // The first parameter interaction. This is what the sweep does and what the
    // timing points at.
    auto params = inst->getParameters();
    if (! params.isEmpty())
    {
        std::cout << "\nwriting parameter 0 (" << params[0]->getName (32) << ") ..." << std::endl;
        params[0]->beginChangeGesture();
        params[0]->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, params[0]->getValue() + 0.1f));
        params[0]->endChangeGesture();
    }

    for (int i = 0; i < seconds * 4; ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);
        const auto s = windowsNow (seen, self);
        if (s.isNotEmpty())
            std::cout << "  t+" << (i / 4.0) << "s\n" << s << std::flush;
    }

    std::cout << "\nprobe over. Any NEW WINDOW line above names the process that owns the\n"
                 "alert: this one, or a vendor helper. That decides whether it can be\n"
                 "dismissed from inside ejmap at all." << std::endl;
    inst.reset();
    return 0;
}
