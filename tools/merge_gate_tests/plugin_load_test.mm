/*  Merge gate (6 Sep 2026): do the BUILT bundles load and pass audio, from the
    build directory, BEFORE anything replaces the installed shoot build?
      VST3: hosted through JUCE from its path (findAllTypesForFile).
      AU:   the component is REGISTERED IN-PROCESS from its bundle path
            (AudioComponentRegister on the plist's description + factory) so
            the same JUCE hosting path instantiates it without an install.
    Each: instantiate, prepareToPlay 48k/512, 16 blocks of a -12 dBFS 440 Hz
    sine, output must be finite and non-silent; latency printed.
    Usage: plugin_load_test <path.vst3> <path.component>                    */
// Carbon's Component/Point typedefs clash with juce::Component; JUCE's own
// hosting code uses this rename around the AudioToolbox include, so do we.
#define Point CarbonDummyPointName
#define Component CarbonDummyCompName
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#undef Point
#undef Component
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include <cmath>
#include <cstdio>

static bool registerAU (const juce::File& component, juce::String& ident)
{
    auto* url = CFURLCreateWithFileSystemPath (nullptr, component.getFullPathName().toCFString(), kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = CFBundleCreate (nullptr, url); CFRelease (url);
    if (bundle == nullptr) { std::printf ("  AU: CFBundleCreate failed\n"); return false; }
    auto* comps = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (bundle, CFSTR ("AudioComponents"));
    if (comps == nullptr || CFArrayGetCount (comps) == 0) { std::printf ("  AU: no AudioComponents in Info.plist\n"); return false; }
    auto* d = (CFDictionaryRef) CFArrayGetValueAtIndex (comps, 0);
    auto str = [&] (const char* k) { return juce::String::fromCFString ((CFStringRef) CFDictionaryGetValue (d, juce::String (k).toCFString())); };
    auto code = [&] (const char* k) { auto s = str (k); return (OSType) ((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3]); };
    int version = 0; CFNumberGetValue ((CFNumberRef) CFDictionaryGetValue (d, CFSTR ("version")), kCFNumberIntType, &version);
    AudioComponentDescription desc {}; desc.componentType = code ("type"); desc.componentSubType = code ("subtype"); desc.componentManufacturer = code ("manufacturer");
    auto factory = (AudioComponentFactoryFunction) CFBundleGetFunctionPointerForName (bundle, str ("factoryFunction").toCFString());
    if (factory == nullptr) { std::printf ("  AU: factory %s NOT FOUND (dlopen/link failure)\n", str ("factoryFunction").toRawUTF8()); return false; }
    auto* comp = AudioComponentRegister (&desc, str ("name").toCFString(), (UInt32) version, factory);
    ident = "AudioUnit:Effects/" + str ("type") + "," + str ("subtype") + "," + str ("manufacturer");
    std::printf ("  AU registered from path: %s (%s v%d) -> %s\n", str ("name").toRawUTF8(), str ("factoryFunction").toRawUTF8(), version, comp ? "ok" : "AudioComponentRegister returned null");
    return comp != nullptr;
}

static int host (juce::AudioPluginFormatManager& fm, const juce::String& formatName, const juce::String& ident, const char* label)
{
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats()) if (f->getName() == formatName) f->findAllTypesForFile (found, ident);
    if (found.isEmpty()) { std::printf ("  %s: NOT FOUND by the %s format at %s\n", label, formatName.toRawUTF8(), ident.toRawUTF8()); return 1; }
    juce::String err; auto inst = fm.createPluginInstance (*found[0], 48000.0, 512, err);
    if (inst == nullptr) { std::printf ("  %s: instance REFUSED: %s\n", label, err.toRawUTF8()); return 1; }
    inst->setPlayConfigDetails (2, 2, 48000.0, 512);
    inst->prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    double phase = 0.0, sumSq = 0.0; bool finite = true; int n = 0;
    for (int b = 0; b < 16; ++b)
    {
        for (int i = 0; i < 512; ++i) { const float v = 0.25f * (float) std::sin (phase); phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0; buf.setSample (0, i, v); buf.setSample (1, i, v); }
        midi.clear(); inst->processBlock (buf, midi);
        if (b >= 8) for (int i = 0; i < 512; ++i) { const float o = buf.getSample (0, i); finite = finite && std::isfinite (o); sumSq += (double) o * o; ++n; }
    }
    const double rms = std::sqrt (sumSq / juce::jmax (1, n));
    const bool ok = finite && rms > 1e-4;
    std::printf ("  %s: loaded '%s' (%s), latency %d, output rms %.4f (input 0.1768), finite=%s -> %s\n", label,
                 found[0]->name.toRawUTF8(), found[0]->version.toRawUTF8(), inst->getLatencySamples(), rms, finite ? "yes" : "NO", ok ? "PASSES AUDIO" : "FAIL");
    inst->releaseResources();
    return ok ? 0 : 1;
}

int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("plugin_load_test.mm");
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 3) { std::printf ("usage: plugin_load_test <path.vst3> <path.component>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI init;
    juce::AudioPluginFormatManager fm; juce::addDefaultFormatsToManager (fm);
    int fails = 0;
    fails += host (fm, "VST3", juce::File (argv[1]).getFullPathName(), "VST3");
    juce::String auIdent;
    if (registerAU (juce::File (argv[2]), auIdent)) fails += host (fm, "AudioUnit", auIdent, "AU"); else ++fails;
    std::printf ("%s\n", fails == 0 ? "BOTH FORMATS LOAD AND PASS AUDIO" : "LOAD GATE FAILED");
    return fails;
}
