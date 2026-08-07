/*
  ArchProbe.cpp

  PROBE 1 of the distribution proposal. Not pipeline code: it answers one
  question with a number, the same shape as 67de0d5.

  THE QUESTION. The scan reports "no description for VST3" for all eight Waves
  VST3 shells on this machine, and the proposal turned on whether that is a
  fixable scan gap or a wall. If those shells enumerate, 210 products stop being
  bridged for MAPPING, not only for capture.

  WHAT IT DOES. Asks VST3PluginFormat for the descriptions in a bundle, prints
  what came back, and prints the bundle's Mach-O architectures beside the
  architecture this process is running as -- because that pairing is the whole
  answer and reading either one alone would miss it.

  Run it BOTH WAYS. The point is the difference:

      ejmap-arch-probe "/Library/Audio/Plug-Ins/VST3/WaveShell1-VST3 12.6.vst3"
      arch -x86_64 ejmap-arch-probe "  (same path)  "
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "EjmapViewLayer.h"
#include <iostream>

static juce::String archOfThisProcess()
{
   #if defined (__aarch64__)
    return "arm64";
   #elif defined (__x86_64__)
    return "x86_64";
   #else
    return "?";
   #endif
}

static juce::String archsOfBundle (const juce::File& bundle)
{
    auto macos = bundle.getChildFile ("Contents/MacOS");
    auto bins  = macos.findChildFiles (juce::File::findFiles, false);
    if (bins.isEmpty())
        return "(no binary in Contents/MacOS)";

    juce::ChildProcess p;
    if (! p.start ("lipo -archs \"" + bins.getFirst().getFullPathName() + "\""))
        return "(lipo would not start)";
    return p.readAllProcessOutput().trim();
}

int runModalProbe (const juce::String& identifier, int seconds);

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // PARSE ORACLE. A map in the live corpus parses under Python and fails
    // under juce::JSON -- deterministically, on freshly written bytes -- and
    // nothing in the toolbox could say WHERE. This prints juce's own error,
    // which carries the position.
    if (argc >= 3 && juce::String (argv[1]) == "--parse-json")
    {
        const juce::String path { juce::CharPointer_UTF8 (argv[2]) };
        const juce::File f { path };
        const auto text = f.loadFileAsString();
        std::cout << "bytes " << (int) f.getSize() << "  chars " << text.length() << "\n";
        juce::var out;
        const auto r = juce::JSON::parse (text, out);
        std::cout << (r.wasOk() ? juce::String ("PARSE OK")
                                : "PARSE FAILED: " + r.getErrorMessage()) << std::endl;
        return r.wasOk() ? 0 : 1;
    }

    if (argc >= 3 && juce::String (argv[1]) == "--modal")
        return runModalProbe (juce::String (juce::CharPointer_UTF8 (argv[2])),
                              argc >= 4 ? juce::String (argv[3]).getIntValue() : 20);

    if (argc < 2)
    {
        std::cerr << "usage: ejmap-arch-probe <path to .vst3>" << std::endl;
        return 2;
    }

    // Most vexing parse: the extra parens make this a declaration, not an object.
    const juce::String path { juce::CharPointer_UTF8 (argv[1]) };
    const juce::File bundle { path };
    if (! bundle.exists())
    {
        std::cerr << "no such bundle: " << bundle.getFullPathName() << std::endl;
        return 2;
    }

    std::cout << "bundle          : " << bundle.getFileName() << "\n"
              << "bundle archs    : " << archsOfBundle (bundle) << "\n"
              << "this process    : " << archOfThisProcess() << std::endl;

    juce::VST3PluginFormat fmt;
    juce::OwnedArray<juce::PluginDescription> found;
    const auto t0 = juce::Time::getMillisecondCounter();
    fmt.findAllTypesForFile (found, bundle.getFullPathName());
    const auto ms = juce::Time::getMillisecondCounter() - t0;

    std::cout << "descriptions    : " << found.size() << "   (" << ms << " ms)" << std::endl;

    for (int i = 0; i < juce::jmin (12, found.size()); ++i)
        std::cout << "    " << found[i]->name << "   |   " << found[i]->manufacturerName
                  << "   |   " << found[i]->version << std::endl;
    if (found.size() > 12)
        std::cout << "    … and " << (found.size() - 12) << " more" << std::endl;

    // ---- THE HALF THAT MATTERS -------------------------------------------
    // Enumerating is only worth something if the editor then CAPTURES. A VST3
    // has no out-of-process bridge -- it loads in-process or not at all -- so
    // if this reads a real fraction, the Waves panels are reachable through
    // their VST3 form in a way their AU form is not.
    if (! found.isEmpty() && argc >= 3 && juce::String (argv[2]) == "--capture")
    {
        juce::AudioPluginFormatManager mgr;
        mgr.addFormat (new juce::VST3PluginFormat());
        juce::String err;
        std::unique_ptr<juce::AudioPluginInstance> inst (
            mgr.createPluginInstance (*found[0], 48000.0, 512, err));

        if (inst == nullptr)
        {
            std::cout << "\ninstantiate     : FAILED -- " << err << std::endl;
            return 1;
        }
        std::cout << "\ninstantiated    : " << found[0]->name
                  << "   (" << inst->getParameters().size() << " params)" << std::endl;

        std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
        if (ed == nullptr) { std::cout << "editor          : none" << std::endl; return 1; }

        juce::DocumentWindow win ("probe", juce::Colours::black,
                                  juce::DocumentWindow::closeButton);
        win.setUsingNativeTitleBar (true);
        win.setContentNonOwned (ed.get(), true);
        win.setVisible (true);
        // Let it settle: a panel that has not drawn yet reads empty for a
        // reason that has nothing to do with bridging.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (3000);

        std::cout << "view tree:\n" << ejmap::describeViewTree (win);
        auto out = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("archprobe-capture.png");
        std::cout << "capture         : " << ejmap::captureHostedEditor (win, out) << std::endl;
        win.setContentNonOwned (nullptr, false);
        ed.reset();
    }

    // The verdict is the PAIRING, never either half alone.
    std::cout << "\nVERDICT: " << (found.isEmpty()
        ? "NOTHING ENUMERATED. If the bundle's archs do not include this "
          "process's arch, that is the reason: a VST3 must be dlopen'd IN "
          "PROCESS and there is no bridge for it."
        : "enumerated. This bundle can be hosted in-process by this build.")
              << std::endl;
    return found.isEmpty() ? 1 : 0;
}
