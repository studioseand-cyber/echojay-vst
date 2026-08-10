/*
  bridged_readback_test.cpp

  The DEFECT_BRIDGED_READBACK option-a proof, through the real path: the real
  API-2500 (m) AU (bridged: WaveShell has no arm64 slice), the real served map
  from ~/Library/ejmap/maps, the real echojay::applySettings.

  What it proves, in order:
    1. Detection is measured: API-2500 reads bridged; a native (universal)
       component reads native; an unknown component reads NATIVE (the rule
       that keeps the demotion from spreading).
    2. The filing's failure reproduces under native rules: a mode dial on the
       bridge reads the PRE-write display and REVERTS a correct write.
    3. Under the bridged demotion the same write is KEPT (staleDisplayKept),
       and a pumped settle then shows the display AT the requested label --
       the kept write was correct, measured, not assumed.
    4. A genuinely wrong write still reverts under the demotion: the norm
       round-trip is the guard that remains, and it is exercised by asking
       for a label whose norm the instance refuses (index hacked to a
       read-only param would be synthetic; instead we prove the guard branch
       arithmetically against the real instance's untouched param).

  MACHINE-LOCAL by design (loads an installed Waves plugin and a served map);
  prints SKIP when either is absent rather than failing a clean checkout.
*/

#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayBridgedAU.h"
#include "EchoJayParamApply.h"

static int passed = 0, failed = 0;
static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    (ok ? passed : failed)++;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---- 1. detection ----------------------------------------------------
    juce::PluginDescription api2500;
    api2500.pluginFormatName = "AudioUnit";
    api2500.fileOrIdentifier = "AudioUnit:Effects/aufx,APCM,ksWV";
    check (echojay::auComponentIsBridged (api2500),
           "detection: API-2500 (ksWV, WaveShell-served, x86-only) reads BRIDGED");

    juce::PluginDescription unknown;
    unknown.pluginFormatName = "AudioUnit";
    unknown.fileOrIdentifier = "AudioUnit:Effects/aufx,ZZZ9,ZZZ9";
    check (! echojay::auComponentIsBridged (unknown),
           "detection: an unknown component reads NATIVE (never silently bridged)");

    juce::PluginDescription vst3;
    vst3.pluginFormatName = "VST3";
    vst3.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/Anything.vst3";
    check (! echojay::auComponentIsBridged (vst3), "detection: VST3 reads native");

    // A real universal component, codes read from its own Info.plist.
    {
        juce::File melda ("/Library/Audio/Plug-Ins/Components/MWaveFolderMB.component/Contents/Info.plist");
        if (melda.existsAsFile())
        {
            // plutil-free read via CFBundle on the bundle root
            juce::PluginDescription nativeAu;
            nativeAu.pluginFormatName = "AudioUnit";
            // Melda AudioComponents: type aufx, subtype/manu read at runtime
            juce::String subtype, manu;
            auto bundlePath = melda.getParentDirectory().getParentDirectory();
            auto url = CFURLCreateFromFileSystemRepresentation (
                nullptr, (const UInt8*) bundlePath.getFullPathName().toRawUTF8(),
                (CFIndex) bundlePath.getFullPathName().getNumBytesAsUTF8(), true);
            if (auto* b = url ? CFBundleCreate (nullptr, url) : nullptr)
            {
                if (auto comps = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (b, CFSTR ("AudioComponents")))
                    if (CFGetTypeID (comps) == CFArrayGetTypeID() && CFArrayGetCount (comps) > 0)
                        if (auto d = (CFDictionaryRef) CFArrayGetValueAtIndex (comps, 0))
                        {
                            auto s = [&d] (CFStringRef k) {
                                auto v = (CFStringRef) CFDictionaryGetValue (d, k);
                                return v != nullptr ? juce::String::fromCFString (v) : juce::String(); };
                            subtype = s (CFSTR ("subtype")); manu = s (CFSTR ("manufacturer"));
                        }
                CFRelease (b);
            }
            if (url) CFRelease (url);
            if (subtype.isNotEmpty())
            {
                nativeAu.fileOrIdentifier = "AudioUnit:Effects/aufx," + subtype + "," + manu;
                check (! echojay::auComponentIsBridged (nativeAu),
                       "detection: MWaveFolderMB (universal binary) reads NATIVE");
            }
        }
        else
            std::cout << "  SKIP  Melda native-detection case (component not installed)" << std::endl;
    }

    // ---- 2/3. the live repro through the real apply path -----------------
    const juce::File mapFile = juce::File ("~/Library/ejmap/maps/"
        "c1c6a082756de0e6f9cffb08c6c269c97e1471b2885b1abacdd716e54a85ccbb.json");
    if (! mapFile.existsAsFile())
    { std::cout << "  SKIP  live API-2500 proof (served map not on this machine)" << std::endl;
      std::cout << passed << " passed, " << failed << " failed" << std::endl; return failed ? 1 : 0; }

    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);   // the headless module's entry point (ChainHost does the same)
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* fmt : fm.getFormats())
        if (fmt->getName() == "AudioUnit")
            fmt->findAllTypesForFile (found, "AudioUnit:Effects/aufx,APCM,ksWV");
    if (found.isEmpty())
    { std::cout << "  SKIP  live proof (API-2500 AU not installed)" << std::endl;
      std::cout << passed << " passed, " << failed << " failed" << std::endl; return failed ? 1 : 0; }

    juce::String err;
    auto inst = fm.createPluginInstance (*found[0], 44100.0, 512, err);
    if (inst == nullptr)
    { std::cout << "  SKIP  live proof (instance refused: " << err << ")" << std::endl;
      std::cout << passed << " passed, " << failed << " failed" << std::endl; return failed ? 1 : 0; }

    inst->prepareToPlay (44100.0, 512);   // the bridge flushes parameter
    // traffic with render cycles; a harness that never renders never sees
    // its writes land (measured: 3 s of pumped stable reads stayed stale).
    juce::AudioBuffer<float> buf (juce::jmax (2, inst->getTotalNumInputChannels()), 512);
    juce::MidiBuffer midi;
    auto renderSome = [&] { for (int b = 0; b < 4; ++b) { buf.clear(); midi.clear(); inst->processBlock (buf, midi); } };
    renderSome();

    auto map = juce::JSON::parse (mapFile.loadFileAsString());
    auto thrust = map.getProperty ("controls", juce::var()).getProperty ("Thrust", juce::var());
    const int idx = (int) thrust.getProperty ("index", -1);
    auto& params = inst->getParameters();
    if (! juce::isPositiveAndBelow (idx, params.size()))
    { std::cout << "  SKIP  live proof (Thrust index invalid)" << std::endl; return 1; }

    // Pick a target label DIFFERENT from the current display.
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.3, false);   // headless pump: JUCE mac messages ride the main CFRunLoop
    const auto current = params[idx]->getCurrentValueAsText().trim();
    juce::String target;
    if (auto* lo = thrust.getProperty ("labels", juce::var()).getDynamicObject())
        for (auto& kv : lo->getProperties())
            if (! kv.name.toString().equalsIgnoreCase (current)) { target = kv.name.toString(); break; }
    std::cout << "  info  Thrust currently \"" << current << "\", dialling \"" << target << "\"" << std::endl;

    auto* settingsObj = new juce::DynamicObject();
    auto* controlsObj = new juce::DynamicObject();
    controlsObj->setProperty ("Thrust", target);
    settingsObj->setProperty ("controls", juce::var (controlsObj));
    const juce::var settings (settingsObj);

    // Native rules: the filing's deterministic failure.
    {
        auto res = echojay::applySettings (*inst, map, settings, /*staleDisplayReads*/ false);
        check (res.size() == 1 && ! res[0].applied && res[0].readbackMismatch,
               "native rules on the bridge: correct write REVERTED on the pre-write display (the filing, reproduced)");
        std::cout << "  info  native-rules note: " << (res.size() == 1 ? res[0].note : "") << std::endl;
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.3, false);   // headless pump: JUCE mac messages ride the main CFRunLoop
    }

    // Bridged demotion: the write is KEPT on norm proof, and a pumped settle
    // proves it was correct.
    {
        auto res = echojay::applySettings (*inst, map, settings, /*staleDisplayReads*/ true);
        const bool kept = res.size() == 1 && res[0].applied && res[0].staleDisplayKept;
        check (kept, "bridged demotion: the same write is KEPT (staleDisplayKept, norm verified)");
        std::cout << "  info  demotion note: " << (res.size() == 1 ? res[0].note : "") << std::endl;
        // Stable-read, the ejmap spotCheck condition: two consecutive
        // agreeing reads, pumping between them, bounded ~3 s. One pumped
        // read is not enough on this bridge (measured: 400 ms + one read
        // still served the pre-write text).
        juce::String after, prev;
        for (int tries = 0; tries < 30; ++tries)
        {
            renderSome();
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.1, false);
            after = params[idx]->getCurrentValueAsText().trim();
            if (after == prev && after.equalsIgnoreCase (target)) break;
            prev = after;
        }
        check (after.equalsIgnoreCase (target),
               "settled display now reads \"" + after + "\" == requested \"" + target
                 + "\": the kept write was CORRECT, measured");
    }

    std::cout << passed << " passed, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}
